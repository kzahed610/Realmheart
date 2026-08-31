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

void test_width_contract_is_unchanged() {
    using namespace realmheart::ui::bar;

    for (const auto tier : {
        realmheart::core::DisplayTier::P1080,
        realmheart::core::DisplayTier::P1440,
        realmheart::core::DisplayTier::P4K,
    }) {
        const auto geometry = bar_geometry_for_display_tier(tier);
        require(geometry.rail_width == 56, "taskbar rail width must remain 56");
        require(geometry.cap_extension == 20, "taskbar cap extension must remain 20");
        require(geometry.visual_width == 76, "taskbar visual width must remain 76");
    }
}

void test_tier_height_matches_assigned_logical_resolution() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::bar::bar_geometry_for_logical_geometry;

    require(
        bar_geometry_for_logical_geometry(1920, 1080).surface_height == 1080,
        "1080p taskbar must span 1080 logical pixels"
    );
    require(
        bar_geometry_for_logical_geometry(2560, 1440).surface_height == 1440,
        "1440p taskbar must span 1440 logical pixels"
    );
    require(
        bar_geometry_for_logical_geometry(3840, 2160).surface_height == 2160,
        "4K taskbar must span 2160 logical pixels"
    );
    require(
        bar_geometry_for_logical_geometry(2560, 1200).surface_height == 1200,
        "valid intermediate taskbar geometry must preserve its assigned height"
    );
    require(
        bar_geometry_for_logical_geometry(0, 0).display_tier == DisplayTier::P1080,
        "invalid taskbar geometry must use the 1080p tier"
    );
}

} // namespace

int main() {
    test_width_contract_is_unchanged();
    test_tier_height_matches_assigned_logical_resolution();
    std::cout << "Bar geometry tests passed\n";
    return 0;
}
