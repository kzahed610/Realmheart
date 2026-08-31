#pragma once

#include "core/DisplayTier.hpp"

namespace realmheart::ui::sidebar {

inline constexpr double kSidebarHeightFraction = 0.90;
inline constexpr int kSidebarHotspotHitWidth = 16;

[[nodiscard]] int sidebar_height_for_logical_height(int logical_height) noexcept;

struct SidebarContentLayout {
    int section_spacing = 6;
    int quick_section_margin_bottom = 21;
    int levels_section_margin_bottom = 26;
    int power_section_margin_bottom = 20;
    int quick_grid_spacing = 7;
    int quick_grid_top_inset = 3;
    int quick_tile_height = 64;
    int quick_tile_content_spacing = 10;
    int quick_tile_icon_size = 21;
    int slider_stack_spacing = 2;
    int slider_row_height = 36;
    int slider_row_spacing = 7;
    int slider_icon_size = 17;
    int slider_label_width = 69;
    int slider_value_width = 34;
    int power_segment_height = 33;

    // The released composition lets notifications consume the remaining vertical
    // budget. Higher tiers keep that behaviour after proportionally scaling
    // the modules above it, preserving the same visual rhythm.
    bool notification_expand_to_fill = true;
    int notification_viewport_height = 0;
    int notification_bottom_margin = 30;
    int notification_empty_icon_size = 31;
    int notification_header_spacing = 7;
    int notification_list_spacing = 6;
    int notification_row_spacing = 8;
    int notification_copy_spacing = 4;
    int notification_meta_spacing = 6;
    int notification_unread_dot_size = 5;

    constexpr bool operator==(const SidebarContentLayout&) const noexcept = default;
};

// Logical geometry for the complete sidebar surface. The 1080p values below
// are the released composition from the visual reference: the asymmetric
// 76/42 insets are intentional breathing room for Tessia, not dead space.
// Higher tiers scale the released visible frame proportionally while keeping a
// separately validated character-safe host gutter for Tessia.
struct SidebarFrameLayout {
    int frame_width = 486;
    int character_gutter_width = 240;

    int content_inset_start = 76;
    int content_inset_end = 42;
    int content_inset_top = 38;
    int content_inset_bottom = 38;
    int right_margin = 2;
    int hotspot_hit_width = kSidebarHotspotHitWidth;
    SidebarContentLayout content{};

    [[nodiscard]] constexpr int surface_width() const noexcept {
        return frame_width + character_gutter_width;
    }

    [[nodiscard]] constexpr int frame_origin_x() const noexcept {
        return character_gutter_width;
    }

    [[nodiscard]] constexpr int content_width() const noexcept {
        return frame_width - content_inset_start - content_inset_end;
    }

    constexpr bool operator==(const SidebarFrameLayout&) const noexcept = default;

    [[nodiscard]] static SidebarFrameLayout for_display_tier(
        core::DisplayTier display_tier
    ) noexcept;
};

inline constexpr SidebarFrameLayout kDefaultSidebarFrameLayout{};

} // namespace realmheart::ui::sidebar
