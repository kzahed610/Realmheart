#include "ui/sidebar/SidebarGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::ui::sidebar {
namespace {

int scaled(int value, double scale) noexcept {
    return static_cast<int>(std::lround(static_cast<double>(value) * scale));
}

SidebarContentLayout scaled_content(double scale) noexcept {
    SidebarContentLayout layout;
    layout.section_spacing = scaled(6, scale);
    layout.quick_section_margin_bottom = scaled(21, scale);
    layout.levels_section_margin_bottom = scaled(26, scale);
    layout.power_section_margin_bottom = scaled(20, scale);
    layout.quick_grid_spacing = scaled(7, scale);
    layout.quick_grid_top_inset = scaled(3, scale);
    layout.quick_tile_height = scaled(64, scale);
    layout.quick_tile_content_spacing = scaled(10, scale);
    layout.quick_tile_icon_size = scaled(21, scale);
    layout.slider_stack_spacing = scaled(2, scale);
    layout.slider_row_height = scaled(36, scale);
    layout.slider_row_spacing = scaled(7, scale);
    layout.slider_icon_size = scaled(17, scale);
    layout.slider_label_width = scaled(69, scale);
    layout.slider_value_width = scaled(34, scale);
    layout.power_segment_height = scaled(33, scale);

    // The released 1080p composition intentionally lets the notification card
    // consume the remaining vertical budget. When every preceding module is
    // scaled proportionally, retaining that fill behaviour reproduces the same
    // visual rhythm at scale-1 1440p/4K instead of leaving a giant dead lower
    // half in the forged frame.
    layout.notification_expand_to_fill = true;
    layout.notification_viewport_height = 0;
    layout.notification_bottom_margin = scaled(30, scale);
    layout.notification_empty_icon_size = scaled(31, scale);
    layout.notification_header_spacing = scaled(7, scale);
    layout.notification_list_spacing = scaled(6, scale);
    layout.notification_row_spacing = scaled(8, scale);
    layout.notification_copy_spacing = scaled(4, scale);
    layout.notification_meta_spacing = scaled(6, scale);
    layout.notification_unread_dot_size = std::max(1, scaled(5, scale));
    return layout;
}

SidebarFrameLayout scaled_frame(
    double scale,
    int character_gutter_width
) noexcept {
    SidebarFrameLayout layout;
    layout.frame_width = scaled(486, scale);
    layout.character_gutter_width = character_gutter_width;
    layout.content_inset_start = scaled(76, scale);
    layout.content_inset_end = scaled(42, scale);
    layout.content_inset_top = scaled(38, scale);
    layout.content_inset_bottom = scaled(38, scale);
    layout.right_margin = 2;
    layout.hotspot_hit_width = kSidebarHotspotHitWidth;
    layout.content = scaled_content(scale);
    return layout;
}

} // namespace

int sidebar_height_for_logical_height(int logical_height) noexcept {
    return std::max(
        static_cast<int>(std::lround(
            static_cast<double>(logical_height) * kSidebarHeightFraction
        )),
        1
    );
}

SidebarFrameLayout SidebarFrameLayout::for_display_tier(
    core::DisplayTier display_tier
) noexcept {
    switch (display_tier) {
    case core::DisplayTier::P1440:
        // The visible frame follows the released 1080p composition at 4/3.
        // Tessia keeps the separately validated 340px host clearance rather
        // than shrinking to the exact 320px geometric derivative.
        return scaled_frame(4.0 / 3.0, 340);
    case core::DisplayTier::P4K:
        // Same rule at 2x. The 500px host is slightly wider than the exact
        // 480px derivative because authored hair/hand bounds need the safety.
        return scaled_frame(2.0, 500);
    case core::DisplayTier::P1080:
    default:
        return SidebarFrameLayout{};
    }
}

} // namespace realmheart::ui::sidebar
