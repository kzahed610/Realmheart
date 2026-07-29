// SPDX-License-Identifier: GPL-3.0-or-later
// Render-pass structure adapted from  hyprfx (commit
// d680dabdd2d9362626ecedcad9bd396508163468), itself derived from xhos/hyprfx.

#include "RealmheartVoidPassElement.hpp"

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <utility>

using namespace Render::GL;

namespace {

constexpr float kGold[3] = {0.886F, 0.769F, 0.427F};
constexpr float kStarlight[3] = {0.745F, 0.890F, 1.0F};
constexpr float kAstral[3] = {0.353F, 0.290F, 0.612F};
constexpr float kVoid[3] = {0.024F, 0.031F, 0.094F};

} // namespace

void SRealmheartVoidShader::destroy() {
    if (program != 0) {
        glDeleteProgram(program);
        program = 0;
    }
}

CRealmheartVoidPassElement::CRealmheartVoidPassElement(SData data)
    : m_data(std::move(data)) {}

std::vector<UP<IPassElement>> CRealmheartVoidPassElement::draw() {
    if (m_data.box.width <= 0 || m_data.box.height <= 0 || m_data.texture == 0 ||
        m_data.shader == nullptr || m_data.shader->program == 0) {
        return {};
    }

    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!monitor)
        return {};

    CBox renderBox = m_data.box;
    renderBox.translate(-monitor->m_position).scale(monitor->m_scale);

    const Mat3x3 projection = g_pHyprRenderer->projectBoxToTarget(renderBox);
    const auto& shader = *m_data.shader;

    GLint previousProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);

    g_pHyprRenderer->blend(true);
    glUseProgram(shader.program);

    glUniformMatrix3fv(
        shader.location(ERealmheartVoidUniform::Projection),
        1,
        GL_TRUE,
        projection.getMatrix().data()
    );
    glUniform1f(shader.location(ERealmheartVoidUniform::Progress), m_data.progress);
    glUniform2f(
        shader.location(ERealmheartVoidUniform::Resolution),
        static_cast<float>(m_data.box.width),
        static_cast<float>(m_data.box.height)
    );
    glUniform1f(shader.location(ERealmheartVoidUniform::Radius), m_data.rounding);
    glUniform1f(
        shader.location(ERealmheartVoidUniform::Reverse),
        m_data.reverse ? 1.0F : 0.0F
    );
    glUniform3fv(shader.location(ERealmheartVoidUniform::Gold), 1, kGold);
    glUniform3fv(shader.location(ERealmheartVoidUniform::Starlight), 1, kStarlight);
    glUniform3fv(shader.location(ERealmheartVoidUniform::Astral), 1, kAstral);
    glUniform3fv(shader.location(ERealmheartVoidUniform::Void), 1, kVoid);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(m_data.textureTarget, m_data.texture);
    glTexParameteri(m_data.textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(m_data.textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(shader.location(ERealmheartVoidUniform::Texture), 0);

    static const float positions[] = {
        0.0F, 0.0F,
        1.0F, 0.0F,
        0.0F, 1.0F,
        1.0F, 1.0F,
    };

    const GLint positionLocation = shader.location(ERealmheartVoidUniform::Position);
    glVertexAttribPointer(positionLocation, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glEnableVertexAttribArray(positionLocation);

    g_pHyprRenderer->m_renderData.damage.forEachRect([&](const auto& rectangle) {
        g_pHyprOpenGL->scissor(
            &rectangle,
            g_pHyprRenderer->m_renderData.transformDamage
        );
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    });

    glDisableVertexAttribArray(positionLocation);
    g_pHyprOpenGL->scissor(nullptr);
    glBindTexture(m_data.textureTarget, 0);
    glUseProgram(previousProgram);

    return {};
}

bool CRealmheartVoidPassElement::needsLiveBlur() {
    return false;
}

bool CRealmheartVoidPassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> CRealmheartVoidPassElement::boundingBox() {
    const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!monitor)
        return std::nullopt;

    CBox box = m_data.box;
    return box.translate(-monitor->m_position).expand(2);
}

bool CRealmheartVoidPassElement::disableSimplification() {
    return true;
}
