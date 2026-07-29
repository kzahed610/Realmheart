#pragma once

// SPDX-License-Identifier: GPL-3.0-or-later
// Render-pass structure adapted from  hyprfx (commit
// d680dabdd2d9362626ecedcad9bd396508163468), itself derived from xhos/hyprfx.

#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/Shader.hpp>

#include <array>
#include <cstddef>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

enum class ERealmheartVoidUniform : std::size_t {
    Projection = 0,
    Position,
    Progress,
    Resolution,
    Texture,
    Radius,
    Reverse,
    Gold,
    Starlight,
    Astral,
    Void,
    Count,
};

struct SRealmheartVoidShader {
    GLuint program = 0;
    std::array<GLint, static_cast<std::size_t>(ERealmheartVoidUniform::Count)> locations{};

    [[nodiscard]] GLint location(ERealmheartVoidUniform uniform) const {
        return locations.at(static_cast<std::size_t>(uniform));
    }

    void destroy();
};

class CRealmheartVoidPassElement final : public IPassElement {
  public:
    struct SData {
        CBox box;
        float progress = 0.0F;
        GLuint texture = 0;
        GLenum textureTarget = GL_TEXTURE_2D;
        float rounding = 0.0F;
        bool reverse = false;
        const SRealmheartVoidShader* shader = nullptr;
    };

    explicit CRealmheartVoidPassElement(SData data);
    ~CRealmheartVoidPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;
    bool needsLiveBlur() override;
    bool needsPrecomputeBlur() override;
    std::optional<CBox> boundingBox() override;
    bool disableSimplification() override;

    const char* passName() override {
        return "CRealmheartVoidPassElement";
    }

    ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    SData m_data;
};
