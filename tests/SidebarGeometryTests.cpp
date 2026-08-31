#include "core/DisplayTier.hpp"
#include "ui/sidebar/SidebarGeometry.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_1080p_baseline_is_locked() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::sidebar::SidebarFrameLayout;

    const auto layout = SidebarFrameLayout::for_display_tier(DisplayTier::P1080);
    require(layout.frame_width == 486, "1080p frame width must remain 486");
    require(layout.character_gutter_width == 240, "1080p gutter must remain 240");
    require(layout.surface_width() == 726, "1080p surface width must remain 726");
    require(layout.content_inset_start == 76, "1080p start inset must remain 76");
    require(layout.content_inset_end == 42, "1080p end inset must remain 42");
    require(layout.content_inset_top == 38, "1080p top inset must remain 38");
    require(layout.content_inset_bottom == 38, "1080p bottom inset must remain 38");
    require(layout.right_margin == 2, "1080p right margin must remain 2");
}

void test_higher_tiers_are_shared_scale_derivatives() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::sidebar::SidebarFrameLayout;

    const auto p1440 = SidebarFrameLayout::for_display_tier(DisplayTier::P1440);
    require(p1440.frame_width == 648, "1440p frame must be the 4/3 derivative");
    require(p1440.character_gutter_width == 320, "1440p gutter must be the 4/3 derivative");
    require(p1440.surface_width() == 968, "1440p surface must match frame plus gutter");
    require(p1440.content_inset_start == 101, "1440p start inset must be rounded from 76");
    require(p1440.content_inset_end == 56, "1440p end inset must be rounded from 42");
    require(p1440.content_inset_top == 51, "1440p top inset must be rounded from 38");
    require(p1440.content_inset_bottom == 51, "1440p bottom inset must be rounded from 38");
    require(p1440.right_margin == 3, "1440p right margin must be rounded from 2");

    const auto p4k = SidebarFrameLayout::for_display_tier(DisplayTier::P4K);
    require(p4k.frame_width == 972, "4K frame must be the 2x derivative");
    require(p4k.character_gutter_width == 480, "4K gutter must be the 2x derivative");
    require(p4k.surface_width() == 1452, "4K surface must match frame plus gutter");
    require(p4k.content_inset_start == 152, "4K start inset must be the 2x derivative");
    require(p4k.content_inset_end == 84, "4K end inset must be the 2x derivative");
    require(p4k.content_inset_top == 76, "4K top inset must be the 2x derivative");
    require(p4k.content_inset_bottom == 76, "4K bottom inset must be the 2x derivative");
    require(p4k.right_margin == 4, "4K right margin must be the 2x derivative");
}

void test_content_width_and_right_edge_are_coupled() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::sidebar::SidebarFrameLayout;

    for (const auto tier : {DisplayTier::P1080, DisplayTier::P1440, DisplayTier::P4K}) {
        const auto layout = SidebarFrameLayout::for_display_tier(tier);
        require(
            layout.content_width() ==
                layout.frame_width - layout.content_inset_start - layout.content_inset_end,
            "content width must remain derived from the visible frame"
        );
        require(
            layout.frame_origin_x() + layout.frame_width + layout.right_margin ==
                layout.surface_width() + layout.right_margin,
            "frame and surface must preserve right-edge anchoring"
        );
    }
}

} // namespace

int main() {
    test_1080p_baseline_is_locked();
    test_higher_tiers_are_shared_scale_derivatives();
    test_content_width_and_right_edge_are_coupled();
    std::cout << "Sidebar geometry tests passed\n";
    return 0;
}
