#pragma once

#include "core/DisplayTier.hpp"

namespace realmheart::ui::bar {

inline constexpr int kRailWidth = 56;
inline constexpr int kCapExtension = 20;

// Total surface width of the Aether Spine, including the outward rounded caps.
inline constexpr int kVisualWidth = kRailWidth + kCapExtension;

inline constexpr int kCurveHeight = 35;

struct BarGeometry {
    core::DisplayTier display_tier = core::DisplayTier::P1080;
    int surface_height = 1080;
    int rail_width = kRailWidth;
    int cap_extension = kCapExtension;
    int visual_width = kVisualWidth;
    int curve_height = kCurveHeight;
};

[[nodiscard]] inline BarGeometry bar_geometry_for_display_tier(
    core::DisplayTier display_tier
) noexcept {
    const auto spec = core::display_tier_spec(display_tier);
    return BarGeometry{
        .display_tier = display_tier,
        .surface_height = spec.logical_height,
        .rail_width = kRailWidth,
        .cap_extension = kCapExtension,
        .visual_width = kVisualWidth,
        .curve_height = kCurveHeight,
    };
}

[[nodiscard]] inline BarGeometry bar_geometry_for_logical_geometry(
    int width,
    int height
) noexcept {
    const auto display_tier = core::display_tier_for_logical_geometry(width, height);
    auto geometry = bar_geometry_for_display_tier(display_tier);
    if (width > 0 && height > 0) geometry.surface_height = height;
    return geometry;
}

} // namespace realmheart::ui::bar
