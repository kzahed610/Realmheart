#pragma once

#include <algorithm>
#include <array>
#include <string>
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

// Authored once against the 1920x1080 base. This rectangle is the measured
// non-opaque (alpha<255, AA-inclusive) bounding box of the portal opening in
// the 1080p tier base (see manifest portal_transparent_bbox per tier). The
// mask asset owns the curved silhouette inside this rectangle; runtime code
// owns only cover scaling. Tiers are uniform upscales of the 1080p base, so
// this single design-space rect scales cleanly to 1440p (x1.3333) and 4k (x2).
inline constexpr RectF kPortalViewport{
    804.0F,
    215.0F,
    313.0F,
    437.0F,
};

// One broken-arch stone fragment. Position/size are authored against the same
// 1920x1080 design space as kPortalViewport: the manifest crop rect (padded
// extraction box from the 1672x941 source composition) mapped through the
// 1920/1672 x 1080/941 design scale. The 1080p sprite PNG is the design-space
// size; higher tiers are uniform upscales. Draw rects are top-left anchored.
struct FragmentSpec {
    std::string_view name;
    // Design-space bounding rect of the full sprite PNG (padding included).
    RectF idle_rect;
    // Tiered asset file name under <tier>/fragments/.
    std::string_view file;
};

// Idle layout for the four broken fragments. Positions are the authored spot
// each piece occupies in the source composition (the "fallen/resting" state);
// Phase 4 adds explicit socket targets for reconstruction.
inline constexpr std::array<FragmentSpec, 4> kFragmentSpecs{{
    {"top-right", {1097.8F, 184.8F, 101.0F, 100.0F}, "top-right.png"},
    {"middle-right", {1179.3F, 269.7F, 148.0F, 142.0F}, "middle-right.png"},
    {"lower-right", {1270.0F, 497.0F, 210.0F, 173.0F}, "lower-right.png"},
    {"bottom-left", {494.9F, 488.9F, 168.0F, 141.0F}, "bottom-left.png"},
}};

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

// Fragment sprites follow the same tier layout as the base: each tier folder
// owns a <tier>/fragments/ subdirectory containing the four broken pieces.
[[nodiscard]] inline std::string fragment_asset_relative_path(
    AssetTier tier,
    const FragmentSpec& fragment
) noexcept {
    std::string prefix;
    switch (tier) {
    case AssetTier::P1080:
        prefix = "1080p/fragments/";
        break;
    case AssetTier::P1440:
        prefix = "1440p/fragments/";
        break;
    case AssetTier::P2160:
        prefix = "4k/fragments/";
        break;
    }
    prefix.append(fragment.file);
    return prefix;
}

} // namespace realmheart::relictombs
