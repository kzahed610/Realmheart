#pragma once

#include "core/DisplayTier.hpp"

#include <cmath>

namespace realmheart::ui::launcher {

// Logical launcher geometry. 1080p is the released baseline. Higher tiers are
// intentional proportional enlargements so a scale-1 1440p/4K output keeps the
// same visual density instead of rendering the 1080p launcher as a tiny island.
struct LauncherGeometry {
    double design_scale = 1.0;

    int normal_results_max_height = 336;
    int clipboard_results_max_height = 540;
    int emoji_results_max_height = 540;

    int constellation_node_width = 88;
    int constellation_node_height = 74;
    double constellation_left_inset = 84.0;
    double constellation_right_inset = 56.0;
    double constellation_top_inset = 380.0;
    double constellation_bottom_inset = 58.0;
    double centre_keepout_side = 14.0;
    double centre_keepout_bottom = 44.0;
    int selection_indicator_width = 102;
    int selection_indicator_height = 88;
    double emergence_edge_inset = 58.0;
    double emergence_peek = 8.0;
    double emergence_arc = 18.0;

    int centre_final_top_margin = 166;
    int centre_start_top_margin = 132;
    int centre_final_width = 648;
    int centre_start_width = 600;
    int centre_height = 200;
    int aperture_final_width = 610;
    int aperture_start_width = 548;
    int aperture_final_height = 162;
    int aperture_start_height = 58;
    int search_final_width = 360;
    int search_start_width = 326;
    int search_height = 50;
    int wallpaper_shade_height = 110;
    int results_shell_width = 520;
    int activation_sweep_width = 92;
    int activation_sweep_height = 2;

    int constellation_icon_size = 34;
    int result_icon_size = 36;
    int result_placeholder_icon_size = 30;
    int emoji_glyph_extent = 42;
    int clipboard_thumbnail_width = 104;
    int clipboard_thumbnail_height = 66;

    [[nodiscard]] int scale_px(int baseline) const noexcept {
        return static_cast<int>(std::lround(
            static_cast<double>(baseline) * design_scale
        ));
    }

    constexpr bool operator==(const LauncherGeometry&) const noexcept = default;
};

[[nodiscard]] LauncherGeometry launcher_geometry_for_display_tier(
    core::DisplayTier display_tier
) noexcept;

} // namespace realmheart::ui::launcher
