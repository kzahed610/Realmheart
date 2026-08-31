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

void test_1080p_visual_contract_is_locked() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::sidebar::SidebarFrameLayout;

    const auto layout = SidebarFrameLayout::for_display_tier(DisplayTier::P1080);
    require(layout.frame_width == 486, "1080p visible frame width must remain 486");
    require(layout.character_gutter_width == 240,
            "1080p character host gutter must remain 240");
    require(layout.surface_width() == 726, "1080p surface must remain 726");
    require(layout.content_inset_start == 76,
            "1080p asymmetric Tessia-side inset must remain 76");
    require(layout.content_inset_end == 42,
            "1080p right frame inset must remain 42");
    require(layout.content_width() == 368, "1080p content width must remain 368");
    require(layout.content_inset_top == 38, "1080p top inset must remain 38");
    require(layout.content_inset_bottom == 38, "1080p bottom inset must remain 38");
    require(layout.right_margin == 2, "1080p right margin must remain 2");
    require(layout.hotspot_hit_width == 16, "1080p edge hotspot must remain 16");
    require(layout.content.quick_tile_height == 64, "1080p tile height must remain 64");
    require(layout.content.quick_section_margin_bottom == 21,
            "1080p quick-section rhythm must remain 21");
    require(layout.content.levels_section_margin_bottom == 26,
            "1080p levels-section rhythm must remain 26");
    require(layout.content.power_section_margin_bottom == 20,
            "1080p power-section rhythm must remain 20");
    require(layout.content.quick_grid_top_inset == 3,
            "1080p quick-grid top inset must remain 3");
    require(layout.content.notification_expand_to_fill,
            "1080p notifications must retain released fill behaviour");
    require(layout.content.notification_bottom_margin == 30,
            "1080p notification bottom margin must remain 30");
}

void test_higher_tiers_preserve_released_visual_density() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::sidebar::SidebarFrameLayout;

    const auto p1440 = SidebarFrameLayout::for_display_tier(DisplayTier::P1440);
    require(p1440.frame_width == 648, "1440p frame must be 4/3 of released baseline");
    require(p1440.character_gutter_width == 340,
            "1440p Tessia host must preserve validated bounds");
    require(p1440.surface_width() == 988, "1440p surface must combine frame and safe host");
    require(p1440.content_inset_start == 101, "1440p start inset must scale from baseline");
    require(p1440.content_inset_end == 56, "1440p end inset must scale from baseline");
    require(p1440.content_width() == 491, "1440p content width must preserve baseline density");
    require(p1440.content.quick_tile_height == 85, "1440p controls must scale from baseline");
    require(p1440.content.notification_expand_to_fill,
            "1440p notification card must retain released fill composition");
    require(p1440.content.notification_bottom_margin == 40,
            "1440p notification bottom margin must scale");

    const auto p4k = SidebarFrameLayout::for_display_tier(DisplayTier::P4K);
    require(p4k.frame_width == 972, "4K frame must be 2x released baseline");
    require(p4k.character_gutter_width == 500,
            "4K Tessia host must preserve validated bounds");
    require(p4k.surface_width() == 1472, "4K surface must combine frame and safe host");
    require(p4k.content_inset_start == 152, "4K start inset must be 2x baseline");
    require(p4k.content_inset_end == 84, "4K end inset must be 2x baseline");
    require(p4k.content_width() == 736, "4K content width must be 2x baseline");
    require(p4k.content.quick_tile_height == 128, "4K controls must be 2x baseline");
    require(p4k.content.notification_expand_to_fill,
            "4K notification card must retain released fill composition");
    require(p4k.content.notification_bottom_margin == 60,
            "4K notification bottom margin must be 2x baseline");
}

void test_surface_hotspot_and_right_edge_share_one_contract() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::sidebar::SidebarFrameLayout;

    for (const auto tier : {DisplayTier::P1080, DisplayTier::P1440, DisplayTier::P4K}) {
        const auto layout = SidebarFrameLayout::for_display_tier(tier);
        require(layout.content_width() ==
                    layout.frame_width - layout.content_inset_start - layout.content_inset_end,
                "content width must remain derived from the visible frame");
        require(layout.frame_origin_x() + layout.frame_width == layout.surface_width(),
                "character host and visible frame must share one surface width");
        require(layout.hotspot_hit_width > 0,
                "edge hotspot must be a positive geometry contract value");
        require(layout.hotspot_hit_width < layout.frame_width,
                "edge hotspot must remain narrower than the visible panel");
        require(layout.right_margin == 2,
                "all tiers must preserve the authored right-edge anchoring margin");
    }
}

void test_sidebar_height_uses_the_shared_logical_contract() {
    using realmheart::ui::sidebar::sidebar_height_for_logical_height;

    require(sidebar_height_for_logical_height(1080) == 972,
            "1080p sidebar host height must remain 90% of the logical output");
    require(sidebar_height_for_logical_height(1440) == 1296,
            "1440p sidebar host height must remain 90% of the logical output");
    require(sidebar_height_for_logical_height(2160) == 1944,
            "4K sidebar host height must remain 90% of the logical output");
}

} // namespace

int main() {
    test_1080p_visual_contract_is_locked();
    test_higher_tiers_preserve_released_visual_density();
    test_surface_hotspot_and_right_edge_share_one_contract();
    test_sidebar_height_uses_the_shared_logical_contract();
    std::cout << "Sidebar geometry tests passed\n";
    return 0;
}
