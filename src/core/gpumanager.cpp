/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Xaver Hugl <xaver.huglkde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "gpumanager.h"
#include "drmdevice.h"
#include "utils/common.h"
#include "utils/envvar.h"
#include "utils/udev.h"

#include <QSocketNotifier>
#include <sys/stat.h>

namespace KWin
{

std::unique_ptr<GpuManager> GpuManager::s_self;

GpuManager *GpuManager::self()
{
    return s_self.get();
}

GpuManager::GpuManager()
    : m_udev(std::make_unique<Udev>())
    , m_udevMonitor(m_udev->createMonitor())
    , m_udevNotifier(std::make_unique<QSocketNotifier>(m_udevMonitor->fd(), QSocketNotifier::Read))
{
    m_udevMonitor->filterSubsystemDevType("drm");
    connect(m_udevNotifier.get(), &QSocketNotifier::activated, this, &GpuManager::handleUdevEvent);
    m_udevMonitor->enable();
    scanForRenderDevices();
}

GpuManager::~GpuManager()
{
    m_renderDevices.clear();
}

const std::vector<std::unique_ptr<RenderDevice>> &GpuManager::renderDevices() const
{
    return m_renderDevices;
}

RenderDevice *GpuManager::compatibleRenderDevice(DrmDevice *dev) const
{
    return compatibleRenderDevice(dev->deviceId());
}

RenderDevice *GpuManager::compatibleRenderDevice(dev_t id) const
{
    auto it = m_compatibleDeviceMap.find(id);
    return it == m_compatibleDeviceMap.end() ? nullptr : it.value();
}

RenderDevice *GpuManager::findDevice(dev_t id) const
{
    const auto it = std::ranges::find_if(m_renderDevices, [id](const auto &device) {
        return device->drmDevice()->deviceId() == id;
    });
    return it == m_renderDevices.end() ? nullptr : it->get();
}

void GpuManager::updateCompatibilityMap()
{
    m_compatibleDeviceMap.clear();

    int numberOfDevices = drmGetDevices2(0, nullptr, 0);
    if (!numberOfDevices) {
        return;
    }
    std::vector<drmDevicePtr> devices;
    devices.resize(numberOfDevices);
    if (drmGetDevices2(0, devices.data(), numberOfDevices) < 0) {
        qCWarning(KWIN_CORE, "drmGetDevices2 failed: %s", strerror(errno));
        return;
    }
    for (drmDevicePtr device : devices) {
        RenderDevice *render = findCompatibleRenderDevice(device);
        if (device->available_nodes & (1 << DRM_NODE_PRIMARY)) {
            struct stat buf;
            if (stat(device->nodes[DRM_NODE_PRIMARY], &buf) == -1) {
                qCWarning(KWIN_CORE) << "Failed to fstat drm fd" << device->nodes[DRM_NODE_PRIMARY];
            } else {
                m_compatibleDeviceMap[buf.st_rdev] = render;
            }
        }
        if (device->available_nodes & (1 << DRM_NODE_RENDER)) {
            struct stat buf;
            if (stat(device->nodes[DRM_NODE_RENDER], &buf) == -1) {
                qCWarning(KWIN_CORE) << "Failed to fstat drm fd" << device->nodes[DRM_NODE_RENDER];
            } else {
                m_compatibleDeviceMap[buf.st_rdev] = render;
            }
        }
    }
}

RenderDevice *GpuManager::findCompatibleRenderDevice(drmDevicePtr device) const
{
    auto matchingIt = std::ranges::find_if(m_renderDevices, [device](const auto &renderDevice) {
        return drmDevicesEqual(device, renderDevice->drmDevice()->libdrmDevice()) == 1;
    });
    if (matchingIt != m_renderDevices.end()) {
        return matchingIt->get();
    }
    // fallback for split display/render cases: devices with bus "platform" can be assumed to be compatible
    if (device->bustype == DRM_BUS_PLATFORM) {
        matchingIt = std::ranges::find_if(m_renderDevices, [](const auto &renderDevice) {
            return renderDevice->drmDevice()->busType() == DRM_BUS_PLATFORM;
        });
        if (matchingIt != m_renderDevices.end()) {
            return matchingIt->get();
        }
    }
    return nullptr;
}

void GpuManager::scanForRenderDevices()
{
    const auto devices = m_udev->listRenderNodes();
    for (const auto &udevDevice : devices) {
        if (findDevice(udevDevice->devNum())) {
            continue;
        }
        auto device = RenderDevice::open(udevDevice->devNode());
        if (!device) {
            continue;
        }
        qCDebug(KWIN_CORE, "Adding render device %s", qPrintable(device->drmDevice()->path()));
        m_renderDevices.push_back(std::move(device));
        updateCompatibilityMap();
        Q_EMIT renderDeviceAdded(m_renderDevices.back().get());
    }
}

void GpuManager::handleUdevEvent()
{
    while (auto udevDevice = m_udevMonitor->getDevice()) {
        if (udevDevice->devNode().contains(DRM_PRIMARY_MINOR_NAME)) {
            if (udevDevice->action() == QLatin1StringView("add") || udevDevice->action() == QLatin1StringView("remove")) {
                updateCompatibilityMap();
            }
            continue;
        }
        if (!udevDevice->devNode().contains(DRM_RENDER_MINOR_NAME)) {
            // not a render node, should be ignored
            continue;
        }
        const auto renderDevIt = std::ranges::find_if(m_renderDevices, [&udevDevice](const auto &device) {
            return udevDevice->devNum() == device->drmDevice()->deviceId();
        });
        if (udevDevice->action() == QLatin1StringView("add")) {
            if (renderDevIt != m_renderDevices.end()) {
                continue;
            }
            auto device = RenderDevice::open(udevDevice->devNode());
            if (!device) {
                continue;
            }
            m_renderDevices.push_back(std::move(device));
            updateCompatibilityMap();
            qCDebug(KWIN_CORE, "Added render device %s", qPrintable(m_renderDevices.back()->drmDevice()->path()));
            Q_EMIT renderDeviceAdded(m_renderDevices.back().get());
        } else if (udevDevice->action() == QLatin1StringView("remove")) {
            if (renderDevIt == m_renderDevices.end()) {
                continue;
            }
            auto device = std::move(*renderDevIt);
            m_renderDevices.erase(renderDevIt);
            updateCompatibilityMap();
            qCDebug(KWIN_CORE, "Removed render device %s", qPrintable(device->drmDevice()->path()));
            Q_EMIT renderDeviceRemoved(device.get());
        }
    }
}

}
