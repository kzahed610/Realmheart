// SPDX-License-Identifier: GPL-3.0-or-later
// Upstream provenance for the render-pass structure lives in
// ATTRIBUTION.md.

#include "RealmheartEffectPassElement.hpp"

#include <hyprland/src/render/Context.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

#include <algorithm>
#include <utility>

using namespace Render::GL;

void SRealmheartEffectShader::destroy() {
    if (program != 0) {
        glDeleteProgram(program);
        program = 0;
    }
}

CRealmheartEffectPassElement::CRealmheartEffectPassElement(SData data)
    : m_data(std::move(data)) {}

std::vector<UP<IPassElement>> CRealmheartEffectPassElement::draw(Render::CRenderContext& ctx) {
    if (m_data.box.width <= 0 || m_data.box.height <= 0 || m_data.texture == 0 ||
        m_data.shader == nullptr || m_data.shader->program == 0) {
        return {};
    }

    const auto monitor = ctx.m_data.pMonitor.lock();
    if (!monitor)
        return {};

    CBox renderBox = m_data.box;
    renderBox.translate(-monitor->m_position).scale(monitor->m_scale).round();

    const Mat3x3 projection = g_pHyprRenderer->projectBoxToTarget(ctx, renderBox);
    const auto& shader = *m_data.shader;

    GLint previousProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);

    g_pHyprRenderer->blend(true);
    glUseProgram(shader.program);

    glUniformMatrix3fv(
        shader.location(ERealmheartEffectUniform::Projection),
        1,
        GL_TRUE,
        projection.getMatrix().data()
    );
    glUniform1f(shader.location(ERealmheartEffectUniform::Progress), m_data.progress);
    glUniform2f(
        shader.location(ERealmheartEffectUniform::Resolution),
        static_cast<float>(m_data.box.width),
        static_cast<float>(m_data.box.height)
    );
    glUniform1f(shader.location(ERealmheartEffectUniform::Radius), m_data.rounding);
    glUniform1f(
        shader.location(ERealmheartEffectUniform::Reverse),
        m_data.reverse ? 1.0F : 0.0F
    );
    glUniform1f(
        shader.location(ERealmheartEffectUniform::Opacity),
        std::clamp(m_data.opacity, 0.0F, 1.0F)
    );
    glUniform3fv(
        shader.location(ERealmheartEffectUniform::Gold),
        1,
        m_data.palette.gold.data()
    );
    glUniform3fv(
        shader.location(ERealmheartEffectUniform::Starlight),
        1,
        m_data.palette.starlight.data()
    );
    glUniform3fv(
        shader.location(ERealmheartEffectUniform::Astral),
        1,
        m_data.palette.astral.data()
    );
    glUniform3fv(
        shader.location(ERealmheartEffectUniform::Void),
        1,
        m_data.palette.voidColour.data()
    );

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(m_data.textureTarget, m_data.texture);
    glTexParameteri(m_data.textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(m_data.textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform1i(shader.location(ERealmheartEffectUniform::Texture), 0);

    static const float positions[] = {
        0.0F, 0.0F,
        1.0F, 0.0F,
        0.0F, 1.0F,
        1.0F, 1.0F,
    };

    const GLint positionLocation = shader.location(ERealmheartEffectUniform::Position);
    glVertexAttribPointer(positionLocation, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glEnableVertexAttribArray(positionLocation);

    ctx.m_data.damage.forEachRect([&](const pixman_box32& rectangle) {
        const double left = std::max(static_cast<double>(rectangle.x1), renderBox.x);
        const double top = std::max(static_cast<double>(rectangle.y1), renderBox.y);
        const double right = std::min(
            static_cast<double>(rectangle.x2),
            renderBox.x + renderBox.width
        );
        const double bottom = std::min(
            static_cast<double>(rectangle.y2),
            renderBox.y + renderBox.height
        );
        if (right <= left || bottom <= top)
            return;

        CBox targetDamage{
            left,
            top,
            right - left,
            bottom - top,
        };
        g_pHyprOpenGL->scissor(
            ctx,
            targetDamage,
            ctx.m_data.transformDamage
        );
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    });

    glDisableVertexAttribArray(positionLocation);
    g_pHyprOpenGL->disableScissor();
    glBindTexture(m_data.textureTarget, 0);
    glUseProgram(previousProgram);

    return {};
}

bool CRealmheartEffectPassElement::needsLiveBlur(Render::CRenderContext&) {
    return false;
}

bool CRealmheartEffectPassElement::needsPrecomputeBlur(Render::CRenderContext&) {
    return false;
}

std::optional<CBox> CRealmheartEffectPassElement::boundingBox(Render::CRenderContext& ctx) {
    const auto monitor = ctx.m_data.pMonitor.lock();
    if (!monitor)
        return std::nullopt;

    CBox box = m_data.box;
    return box.translate(-monitor->m_position).expand(2);
}

bool CRealmheartEffectPassElement::disableSimplification(Render::CRenderContext&) {
    return true;
}
