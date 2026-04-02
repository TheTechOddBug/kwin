/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006-2007 Rivo Laks <rivolaks@hot.ee>
    SPDX-FileCopyrightText: 2010, 2011 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2023 Xaver Hugl <xaver.hugl@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "glshadermanager.h"
#include "eglcontext.h"
#include "glplatform.h"
#include "glshader.h"
#include "glvertexbuffer.h"
#include "utils/common.h"

#include <QFile>
#include <QTextStream>

namespace KWin
{

ShaderManager *ShaderManager::instance()
{
    return EglContext::currentContext()->shaderManager();
}

ShaderManager::ShaderManager()
{
}

ShaderManager::~ShaderManager()
{
    while (!m_boundShaders.isEmpty()) {
        popShader();
    }
}

QByteArray ShaderManager::generateVertexSource(ShaderTraits traits) const
{
    QByteArray source;
    QTextStream stream(&source);

    stream << "in vec4 position;\n";
    if (traits & (ShaderTrait::MapTexture | ShaderTrait::MapExternalTexture | ShaderTrait::MapMultiPlaneTexture)) {
        stream << "in vec4 texcoord;\n\n";
        stream << "out vec2 texcoord0;\n\n";
    } else {
        stream << "\n";
    }

    if (traits & (ShaderTrait::RoundedCorners | ShaderTrait::Border)) {
        stream << "out vec2 position0;\n\n";
    }

    stream << "uniform mat4 modelViewProjectionMatrix;\n\n";

    stream << "void main()\n{\n";
    if (traits & (ShaderTrait::MapTexture | ShaderTrait::MapExternalTexture | ShaderTrait::MapMultiPlaneTexture)) {
        stream << "    texcoord0 = texcoord.st;\n";
    }

    if (traits & (ShaderTrait::RoundedCorners | ShaderTrait::Border)) {
        stream << "    position0 = position.xy;\n";
    }

    stream << "    gl_Position = modelViewProjectionMatrix * position;\n";
    stream << "}\n";

    stream.flush();
    return source;
}

QByteArray ShaderManager::generateFragmentSource(ShaderTraits traits) const
{
    QByteArray source;
    QTextStream stream(&source);

    const auto context = EglContext::currentContext();
    if (context->isOpenGLES() && context->glslVersion() < Version(3, 0)) {
        if (traits & (ShaderTrait::RoundedCorners | ShaderTrait::Border)) {
            stream << "#extension GL_OES_standard_derivatives : enable\n\n";
        }
    }

    if (traits & ShaderTrait::MapTexture) {
        stream << "uniform sampler2D sampler;\n";
        stream << "in vec2 texcoord0;\n";
    } else if (traits & ShaderTrait::MapMultiPlaneTexture) {
        stream << "uniform sampler2D sampler;\n";
        stream << "uniform sampler2D sampler1;\n";
        stream << "in vec2 texcoord0;\n";
    } else if (traits & ShaderTrait::MapExternalTexture) {
        stream << "#extension GL_OES_EGL_image_external : require\n\n";
        stream << "uniform samplerExternalOES sampler;\n";
        stream << "in vec2 texcoord0;\n";
    } else if (traits & ShaderTrait::UniformColor) {
        stream << "uniform vec4 geometryColor;\n";
    } else if (traits & ShaderTrait::Border) {
        stream << "#include \"sdf.glsl\"\n";

        stream << "uniform vec4 box;\n";
        stream << "uniform vec4 cornerRadius;\n";
        stream << "uniform vec4 geometryColor;\n";
        stream << "uniform int thickness;\n";
        stream << "in vec2 position0;\n";
    }

    if (traits & ShaderTrait::YuvConversion) {
        stream << "uniform mat4 yuvToRgb;\n";
    }
    if (traits & ShaderTrait::Modulate) {
        stream << "uniform vec4 modulation;\n";
    }
    if (traits & ShaderTrait::AdjustSaturation) {
        stream << "#include \"saturation.glsl\"\n";
    }
    if (traits & ShaderTrait::TransformColorspace) {
        stream << "#include \"colormanagement.glsl\"\n";
    }
    if (traits & ShaderTrait::RoundedCorners) {
        stream << "#include \"sdf.glsl\"\n";

        stream << "uniform vec4 box;\n";
        stream << "uniform vec4 cornerRadius;\n";
        stream << "in vec2 position0;\n";
    }
    stream << "\nout vec4 fragColor;\n";

    stream << "\nvoid main(void)\n{\n";
    stream << "    vec4 result;\n";
    if (traits & ShaderTrait::MapTexture) {
        stream << "    result = texture(sampler, texcoord0);\n";
    } else if (traits & ShaderTrait::MapMultiPlaneTexture) {
        stream << "    result = vec4(texture(sampler, texcoord0).x, texture(sampler1, texcoord0).rg, 1.0);\n";
    } else if (traits & ShaderTrait::MapExternalTexture) {
        // external textures require texture2D for sampling
        stream << "    result = texture2D(sampler, texcoord0);\n";
    } else if (traits & ShaderTrait::UniformColor) {
        stream << "    result = geometryColor;\n";
    } else if (traits & ShaderTrait::Border) {
        stream << "    float inner = sdfRoundedBox(position0, box.xy, box.zw, cornerRadius);\n";
        stream << "    float outer = sdfRoundedBox(position0, box.xy, box.zw + vec2(thickness), cornerRadius + vec4(thickness));\n";
        stream << "    float f = sdfSubtract(outer, inner);\n";
        stream << "    float df = fwidth(f);\n";
        stream << "    result = geometryColor * (1.0 - clamp(0.5 + f / df, 0.0, 1.0));\n";
    }

    if (traits & ShaderTrait::YuvConversion) {
        stream << "result.rgb = (yuvToRgb * vec4(result.rgb, 1.0)).rgb;";
    }
    if (traits & ShaderTrait::RoundedCorners) {
        stream << "    float f = sdfRoundedBox(position0, box.xy, box.zw, cornerRadius);\n";
        stream << "    float df = fwidth(f);\n";
        stream << "    result *= 1.0 - clamp(0.5 + f / df, 0.0, 1.0);\n";
    }
    if (traits & ShaderTrait::TransformColorspace) {
        stream << "    result = encodingToNits(result, sourceNamedTransferFunction, sourceTransferFunctionParams.x, sourceTransferFunctionParams.y);\n";
        stream << "    result.rgb = (colorimetryTransform * vec4(result.rgb, 1.0)).rgb;\n";
    }
    if (traits & ShaderTrait::AdjustSaturation) {
        stream << "    result = adjustSaturation(result);\n";
    }
    if (traits & ShaderTrait::Modulate) {
        stream << "    result *= modulation;\n";
    }
    if (traits & ShaderTrait::TransformColorspace) {
        stream << "    result.rgb = doTonemapping(result.rgb);\n";
        stream << "    result = nitsToDestinationEncoding(result);\n";
    }

    stream << "    fragColor = result;\n";
    stream << "}";
    stream.flush();
    return source;
}

std::unique_ptr<GLShader> ShaderManager::generateShader(ShaderTraits traits)
{
    return generateCustomShader(traits);
}

std::unique_ptr<GLShader> ShaderManager::generateCustomShader(ShaderTraits traits, const QByteArray &vertexSource, const QByteArray &fragmentSource)
{
    const auto vertex = vertexSource.isEmpty() ? generateVertexSource(traits) : vertexSource;
    const auto fragment = fragmentSource.isEmpty() ? generateFragmentSource(traits) : fragmentSource;

    auto shader = std::make_unique<GLShader>();
    if (!shader->load(vertex, fragment)) {
        return nullptr;
    }

    shader->bindAttributeLocation("position", VA_Position);
    shader->bindAttributeLocation("texcoord", VA_TexCoord);
    shader->bindFragDataLocation("fragColor", 0);

    if (!shader->link()) {
        return nullptr;
    }

    return shader;
}

std::unique_ptr<GLShader> ShaderManager::generateShaderFromFile(ShaderTraits traits, const QString &vertexFile, const QString &fragmentFile)
{
    auto loadShaderFile = [](const QString &filePath) {
        QFile file(filePath);
        if (file.open(QIODevice::ReadOnly)) {
            return file.readAll();
        }
        qCCritical(KWIN_OPENGL) << "Failed to read shader " << filePath;
        return QByteArray();
    };
    QByteArray vertexSource;
    QByteArray fragmentSource;
    if (!vertexFile.isEmpty()) {
        vertexSource = loadShaderFile(vertexFile);
        if (vertexSource.isEmpty()) {
            return nullptr;
        }
    }
    if (!fragmentFile.isEmpty()) {
        fragmentSource = loadShaderFile(fragmentFile);
        if (fragmentSource.isEmpty()) {
            return nullptr;
        }
    }
    return generateCustomShader(traits, vertexSource, fragmentSource);
}

GLShader *ShaderManager::shader(ShaderTraits traits)
{
    std::unique_ptr<GLShader> &shader = m_shaderHash[traits];
    if (!shader) {
        shader = generateShader(traits);
    }
    return shader.get();
}

GLShader *ShaderManager::getBoundShader() const
{
    if (m_boundShaders.isEmpty()) {
        return nullptr;
    } else {
        return m_boundShaders.top();
    }
}

bool ShaderManager::isShaderBound() const
{
    return !m_boundShaders.isEmpty();
}

GLShader *ShaderManager::pushShader(ShaderTraits traits)
{
    GLShader *shader = this->shader(traits);
    pushShader(shader);
    return shader;
}

void ShaderManager::pushShader(GLShader *shader)
{
    // only bind shader if it is not already bound
    if (shader != getBoundShader()) {
        shader->bind();
    }
    m_boundShaders.push(shader);
}

void ShaderManager::popShader()
{
    if (m_boundShaders.isEmpty()) {
        return;
    }
    GLShader *shader = m_boundShaders.pop();
    if (m_boundShaders.isEmpty()) {
        // no more shader bound - unbind
        shader->unbind();
    } else if (shader != m_boundShaders.top()) {
        // only rebind if a different shader is on top of stack
        m_boundShaders.top()->bind();
    }
}

void ShaderManager::bindFragDataLocations(GLShader *shader)
{
    shader->bindFragDataLocation("fragColor", 0);
}

void ShaderManager::bindAttributeLocations(GLShader *shader) const
{
    shader->bindAttributeLocation("vertex", VA_Position);
    shader->bindAttributeLocation("texCoord", VA_TexCoord);
}

std::unique_ptr<GLShader> ShaderManager::loadShaderFromCode(const QByteArray &vertexSource, const QByteArray &fragmentSource)
{
    auto shader = std::make_unique<GLShader>();
    if (!shader->load(vertexSource, fragmentSource)) {
        return nullptr;
    }

    bindAttributeLocations(shader.get());
    bindFragDataLocations(shader.get());

    if (!shader->link()) {
        return nullptr;
    }

    return shader;
}

}
