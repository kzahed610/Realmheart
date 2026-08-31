#pragma once

#include "core/DisplayTier.hpp"

namespace realmheart::ui::bar {

// The 1080p Aether Spine is the authored visual reference. Resolution support
// must never reinterpret that baseline: higher tiers scale from these values,
// while the 1080p profile keeps the released silhouette and rhythm intact.
inline constexpr int kRailWidth = 56;
inline constexpr int kCapExtension = 20;
inline constexpr int kVisualWidth = kRailWidth + kCapExtension;
inline constexpr int kCurveHeight = 35;

struct BarGeometry {
    core::DisplayTier display_tier = core::DisplayTier::P1080;
    int surface_height = 1080;
    int rail_width = kRailWidth;
    int cap_extension = kCapExtension;
    int visual_width = kVisualWidth;
    int curve_height = kCurveHeight;

    // Cross-axis layout. Rail padding is selected by the local CSS tier class
    // because GTK exposes margin, not runtime padding. A zero button/pill
    // extent means "keep the authored GTK/CSS natural minimum".
    int icon_button_extent = 0;
    int icon_size = 24;
    int launcher_icon_size = 32;
    int system_pill_width = 0;
    int system_pill_height = 0;
    int system_metric_icon_size = 20;
    int system_metrics_spacing = 8;
    int workspace_stack_width = 36;
    int workspace_stack_padding = 11;
    int workspace_stack_spacing = 5;
    int workspace_rune_width = 34;
    int workspace_rune_height = 38;
    int workspace_art_width = 25;
    int workspace_art_height = 31;
    int separator_width = 28;

    // Vertical composition. These values preserve the original CSS-authored
    // breathing room, but are now explicit so higher tiers can scale without
    // leaking per-child magic margins back into the stylesheet.
    int top_cluster_spacing = 58; // 12px authored gap + 46px control rhythm.
    int top_cluster_margin_top = 7;
    int bottom_cluster_spacing = 14;
    int bottom_cluster_margin_bottom = 10;
    int workspace_section_spacing = 18;
    int status_separator_bottom_margin = 40;
    int notification_bottom_margin = 80;
    int bottom_action_bottom_margin = 6;
};

[[nodiscard]] inline BarGeometry scaled_bar_geometry(
    core::DisplayTier display_tier,
    double scale
) noexcept {
    const auto spec = core::display_tier_spec(display_tier);
    auto s = [scale](int value) { return core::scale_dimension(value, scale); };

    BarGeometry geometry{
        .display_tier = display_tier,
        .surface_height = spec.logical_height,
        .rail_width = s(kRailWidth),
        .cap_extension = s(kCapExtension),
        .visual_width = s(kVisualWidth),
        .curve_height = s(kCurveHeight),
        .icon_button_extent = s(40),
        .icon_size = s(24),
        .launcher_icon_size = s(32),
        .system_pill_width = s(42),
        .system_pill_height = s(118),
        .system_metric_icon_size = s(20),
        .system_metrics_spacing = s(8),
        .workspace_stack_width = s(36),
        .workspace_stack_padding = s(11),
        .workspace_stack_spacing = s(5),
        .workspace_rune_width = s(34),
        .workspace_rune_height = s(38),
        .workspace_art_width = s(25),
        .workspace_art_height = s(31),
        .separator_width = s(28),
        .top_cluster_spacing = s(58),
        .top_cluster_margin_top = s(7),
        .bottom_cluster_spacing = s(14),
        .bottom_cluster_margin_bottom = s(10),
        .workspace_section_spacing = s(18),
        .status_separator_bottom_margin = s(40),
        .notification_bottom_margin = s(80),
        .bottom_action_bottom_margin = s(6),
    };

    // Keep the arithmetic relationship exact even if individual rounded terms
    // would otherwise differ by one pixel.
    geometry.visual_width = geometry.rail_width + geometry.cap_extension;
    return geometry;
}

[[nodiscard]] inline BarGeometry bar_geometry_for_display_tier(
    core::DisplayTier display_tier
) noexcept {
    switch (display_tier) {
    case core::DisplayTier::P1440:
        return scaled_bar_geometry(display_tier, 4.0 / 3.0);
    case core::DisplayTier::P4K:
        return scaled_bar_geometry(display_tier, 2.0);
    case core::DisplayTier::P1080:
    default:
        return BarGeometry{};
    }
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
