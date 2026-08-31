#include "core/DisplayTier.hpp"
#include "ui/launcher/LauncherGeometry.hpp"

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

void test_1080p_launcher_contract_is_unchanged() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::launcher::launcher_geometry_for_display_tier;
    const auto g = launcher_geometry_for_display_tier(DisplayTier::P1080);
    require(g.centre_final_width == 648, "1080p centre width must remain 648");
    require(g.centre_height == 200, "1080p centre height must remain 200");
    require(g.aperture_final_width == 610, "1080p aperture width must remain 610");
    require(g.search_final_width == 360, "1080p search width must remain 360");
    require(g.constellation_node_width == 88, "1080p constellation node width must remain 88");
    require(g.constellation_icon_size == 34, "1080p constellation icon must remain 34");
    require(g.results_shell_width == 520, "1080p result sheet must remain 520");
}

void test_higher_tiers_preserve_visual_density() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::launcher::launcher_geometry_for_display_tier;
    const auto p1440 = launcher_geometry_for_display_tier(DisplayTier::P1440);
    const auto p4k = launcher_geometry_for_display_tier(DisplayTier::P4K);

    require(p1440.centre_final_width == 864, "1440p centre must be 4/3 baseline");
    require(p1440.centre_height == 267, "1440p centre height must scale");
    require(p1440.search_final_width == 480, "1440p search must scale");
    require(p1440.constellation_node_width == 117, "1440p nodes must scale");
    require(p1440.constellation_icon_size == 45, "1440p icons must scale");

    require(p4k.centre_final_width == 1296, "4K centre must be 2x baseline");
    require(p4k.centre_height == 400, "4K centre height must be 2x baseline");
    require(p4k.aperture_final_width == 1220, "4K aperture must be 2x baseline");
    require(p4k.search_final_width == 720, "4K search must be 2x baseline");
    require(p4k.constellation_node_width == 176, "4K nodes must be 2x baseline");
    require(p4k.constellation_icon_size == 68, "4K icons must be 2x baseline");
    require(p4k.results_shell_width == 1040, "4K result sheet must be 2x baseline");
}

} // namespace

int main() {
    test_1080p_launcher_contract_is_unchanged();
    test_higher_tiers_preserve_visual_density();
    std::cout << "Launcher geometry tests passed\n";
    return 0;
}
