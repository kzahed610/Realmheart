#pragma once

#include "core/DisplayTier.hpp"

namespace realmheart::ui::sidebar {

// Logical geometry for the complete sidebar surface. Values are authored for
// 1080p and derived for higher display tiers by the shared tier scale.
struct SidebarFrameLayout {
    int frame_width = 486;
    int character_gutter_width = 240;

    int content_inset_start = 76;
    int content_inset_end = 42;
    int content_inset_top = 38;
    int content_inset_bottom = 38;
    int right_margin = 2;

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
