#pragma once

#include <algorithm>
#include <string_view>

namespace realmheart::relictombs {

inline constexpr float kDesignWidth = 1920.0F;
inline constexpr float kDesignHeight = 1080.0F;

struct RectF {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

struct SceneTransform {
    float scale = 1.0F;
    float offset_x = 0.0F;
    float offset_y = 0.0F;
};

// Authored once against the 1920x1080 base. The mask asset owns the curved
// silhouette inside this rectangle; runtime code owns only cover scaling.
inline constexpr RectF kPortalViewport{
    750.0F,
    195.0F,
    390.0F,
    450.0F,
};

enum class AssetTier {
    P1080,
    P1440,
    P2160,
};

[[nodiscard]] inline SceneTransform make_scene_transform(
    float framebuffer_width,
    float framebuffer_height
) noexcept {
    const float safe_width = std::max(framebuffer_width, 1.0F);
    const float safe_height = std::max(framebuffer_height, 1.0F);
    const float scale = std::max(
        safe_width / kDesignWidth,
        safe_height / kDesignHeight
    );
    return SceneTransform{
        scale,
        (safe_width - kDesignWidth * scale) * 0.5F,
        (safe_height - kDesignHeight * scale) * 0.5F,
    };
}

[[nodiscard]] inline AssetTier select_asset_tier(
    int physical_output_height
) noexcept {
    if (physical_output_height <= 1080) return AssetTier::P1080;
    if (physical_output_height <= 1440) return AssetTier::P1440;
    return AssetTier::P2160;
}

[[nodiscard]] inline std::string_view base_asset_relative_path(
    AssetTier tier
) noexcept {
    switch (tier) {
    case AssetTier::P1080:
        return "1080p/base.png";
    case AssetTier::P1440:
        return "1440p/base.png";
    case AssetTier::P2160:
        return "4k/base.png";
    }
    return "1080p/base.png";
}

} // namespace realmheart::relictombs
