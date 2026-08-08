#pragma once

// SPDX-License-Identifier: GPL-3.0-or-later
// Upstream provenance for the render-pass structure lives in
// ATTRIBUTION.md.

#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/Shader.hpp>

#include <array>
#include <cstddef>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

enum class ERealmheartEffectUniform : std::size_t {
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
    Opacity,
    Count,
};

struct SRealmheartEffectShader {
    GLuint program = 0;
    std::array<GLint, static_cast<std::size_t>(ERealmheartEffectUniform::Count)> locations{};

    [[nodiscard]] GLint location(ERealmheartEffectUniform uniform) const {
        return locations.at(static_cast<std::size_t>(uniform));
    }

    void destroy();
};

struct SRealmheartEffectPalette {
    std::array<float, 3> gold{0.886F, 0.725F, 0.416F};
    std::array<float, 3> starlight{0.790F, 0.845F, 1.0F};
    std::array<float, 3> astral{0.405F, 0.255F, 0.705F};
    std::array<float, 3> voidColour{0.016F, 0.020F, 0.060F};
};

class CRealmheartEffectPassElement final : public IPassElement {
  public:
    struct SData {
        CBox box;
        float progress = 0.0F;
        GLuint texture = 0;
        GLenum textureTarget = GL_TEXTURE_2D;
        float rounding = 0.0F;
        bool reverse = false;
        float opacity = 1.0F;
        const SRealmheartEffectShader* shader = nullptr;
        SRealmheartEffectPalette palette{};
    };

    explicit CRealmheartEffectPassElement(SData data);
    ~CRealmheartEffectPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;
    bool needsLiveBlur() override;
    bool needsPrecomputeBlur() override;
    std::optional<CBox> boundingBox() override;
    bool disableSimplification() override;

    const char* passName() override {
        return "CRealmheartEffectPassElement";
    }

    ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    SData m_data;
};
