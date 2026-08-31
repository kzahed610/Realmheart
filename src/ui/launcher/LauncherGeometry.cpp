#include "ui/launcher/LauncherGeometry.hpp"

#include <algorithm>

namespace realmheart::ui::launcher {
namespace {

LauncherGeometry scaled_geometry(double scale) noexcept {
    LauncherGeometry geometry;
    geometry.design_scale = scale;

    const auto s = [scale](int value) {
        return static_cast<int>(std::lround(static_cast<double>(value) * scale));
    };
    const auto sd = [scale](double value) { return value * scale; };

    geometry.normal_results_max_height = s(336);
    geometry.clipboard_results_max_height = s(540);
    geometry.emoji_results_max_height = s(540);

    geometry.constellation_node_width = s(88);
    geometry.constellation_node_height = s(74);
    geometry.constellation_left_inset = sd(84.0);
    geometry.constellation_right_inset = sd(56.0);
    geometry.constellation_top_inset = sd(380.0);
    geometry.constellation_bottom_inset = sd(58.0);
    geometry.centre_keepout_side = sd(14.0);
    geometry.centre_keepout_bottom = sd(44.0);
    geometry.selection_indicator_width = s(102);
    geometry.selection_indicator_height = s(88);
    geometry.emergence_edge_inset = sd(58.0);
    geometry.emergence_peek = sd(8.0);
    geometry.emergence_arc = sd(18.0);

    geometry.centre_final_top_margin = s(166);
    geometry.centre_start_top_margin = s(132);
    geometry.centre_final_width = s(648);
    geometry.centre_start_width = s(600);
    geometry.centre_height = s(200);
    geometry.aperture_final_width = s(610);
    geometry.aperture_start_width = s(548);
    geometry.aperture_final_height = s(162);
    geometry.aperture_start_height = s(58);
    geometry.search_final_width = s(360);
    geometry.search_start_width = s(326);
    geometry.search_height = s(50);
    geometry.wallpaper_shade_height = s(110);
    geometry.results_shell_width = s(520);
    geometry.activation_sweep_width = s(92);
    geometry.activation_sweep_height = std::max(1, s(2));

    geometry.constellation_icon_size = s(34);
    geometry.result_icon_size = s(36);
    geometry.result_placeholder_icon_size = s(30);
    geometry.emoji_glyph_extent = s(42);
    geometry.clipboard_thumbnail_width = s(104);
    geometry.clipboard_thumbnail_height = s(66);
    return geometry;
}

} // namespace

LauncherGeometry launcher_geometry_for_display_tier(
    core::DisplayTier display_tier
) noexcept {
    switch (display_tier) {
    case core::DisplayTier::P1440:
        return scaled_geometry(4.0 / 3.0);
    case core::DisplayTier::P4K:
        return scaled_geometry(2.0);
    case core::DisplayTier::P1080:
    default:
        return LauncherGeometry{};
    }
}

} // namespace realmheart::ui::launcher
