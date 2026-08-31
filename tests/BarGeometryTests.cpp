#include "core/DisplayTier.hpp"
#include "ui/bar/BarGeometry.hpp"

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
    using realmheart::ui::bar::bar_geometry_for_display_tier;

    const auto p1080 = bar_geometry_for_display_tier(DisplayTier::P1080);
    require(p1080.rail_width == 56, "1080p rail width must remain 56");
    require(p1080.cap_extension == 20, "1080p cap extension must remain 20");
    require(p1080.visual_width == 76, "1080p visual width must remain 76");
    require(p1080.curve_height == 35, "1080p curve height must remain 35");
    require(p1080.icon_button_extent == 0,
            "1080p icon buttons must retain authored CSS minimums");
    require(p1080.icon_size == 24, "1080p normal icon size must remain 24");
    require(p1080.launcher_icon_size == 32, "1080p launcher icon must remain 32");
    require(p1080.workspace_rune_width == 34, "1080p rune width must remain 34");
    require(p1080.workspace_rune_height == 38, "1080p rune height must remain 38");
    require(p1080.top_cluster_spacing == 58,
            "1080p top rhythm must preserve the authored 12+46 gap");
    require(p1080.status_separator_bottom_margin == 40,
            "1080p status separator breathing room must remain 40");
    require(p1080.notification_bottom_margin == 80,
            "1080p notification-to-power breathing room must remain 80");
}

void test_higher_tiers_scale_from_the_reference_composition() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::bar::bar_geometry_for_display_tier;

    const auto p1440 = bar_geometry_for_display_tier(DisplayTier::P1440);
    require(p1440.rail_width == 75, "1440p rail width must be 75");
    require(p1440.cap_extension == 27, "1440p cap extension must be 27");
    require(p1440.visual_width == 102, "1440p visual width must be 102");
    require(p1440.icon_button_extent == 53, "1440p button extent must be 53");
    require(p1440.icon_size == 32, "1440p normal icon size must be 32");
    require(p1440.launcher_icon_size == 43, "1440p launcher icon must be 43");
    require(p1440.top_cluster_spacing == 77, "1440p top rhythm must scale deliberately");
    require(p1440.notification_bottom_margin == 107,
            "1440p lower breathing room must scale deliberately");

    const auto p4k = bar_geometry_for_display_tier(DisplayTier::P4K);
    require(p4k.rail_width == 112, "4K rail width must be 112");
    require(p4k.cap_extension == 40, "4K cap extension must be 40");
    require(p4k.visual_width == 152, "4K visual width must be 152");
    require(p4k.icon_button_extent == 80, "4K button extent must be 80");
    require(p4k.icon_size == 48, "4K normal icon size must be 48");
    require(p4k.launcher_icon_size == 64, "4K launcher icon must be 64");
    require(p4k.top_cluster_spacing == 116, "4K top rhythm must scale deliberately");
    require(p4k.notification_bottom_margin == 160,
            "4K lower breathing room must scale deliberately");

    require(p1440.visual_width < p4k.visual_width,
            "higher display tiers must receive larger intentional rail budgets");
}

void test_tier_height_matches_assigned_logical_resolution() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::bar::bar_geometry_for_logical_geometry;

    require(bar_geometry_for_logical_geometry(1920, 1080).surface_height == 1080,
            "1080p taskbar must span 1080 logical pixels");
    require(bar_geometry_for_logical_geometry(2560, 1440).surface_height == 1440,
            "1440p taskbar must span 1440 logical pixels");
    require(bar_geometry_for_logical_geometry(3840, 2160).surface_height == 2160,
            "4K taskbar must span 2160 logical pixels");
    require(bar_geometry_for_logical_geometry(2560, 1200).surface_height == 1200,
            "valid intermediate taskbar geometry must preserve its assigned height");
    require(bar_geometry_for_logical_geometry(0, 0).display_tier == DisplayTier::P1080,
            "invalid taskbar geometry must use the 1080p tier");
}

} // namespace

int main() {
    test_1080p_visual_contract_is_locked();
    test_higher_tiers_scale_from_the_reference_composition();
    test_tier_height_matches_assigned_logical_resolution();
    std::cout << "Bar geometry tests passed\n";
    return 0;
}
