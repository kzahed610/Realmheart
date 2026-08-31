#include "core/DisplayTier.hpp"
#include "ui/NotesGeometry.hpp"

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

void test_1080p_notes_baseline_is_preserved() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::notes_layout_for_display_tier;

    const auto layout = notes_layout_for_display_tier(DisplayTier::P1080);
    require(layout.window_width == 600, "1080p notes width must remain 600");
    require(layout.window_height == 800, "1080p notes height must remain 800");
    require(layout.text_margin_horizontal == 18, "1080p horizontal text margin must remain 18");
    require(layout.text_margin_top == 14, "1080p top text margin must remain 14");
    require(layout.text_margin_bottom == 16, "1080p bottom text margin must remain 16");
}

void test_higher_tiers_scale_notes_against_logical_viewport() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::notes_layout_for_display_tier;

    const auto p1440 = notes_layout_for_display_tier(DisplayTier::P1440);
    require(p1440.window_width == 800, "1440p notes width must be 800");
    require(p1440.window_height == 1067, "1440p notes height must be 1067");
    require(p1440.text_margin_horizontal == 24, "1440p horizontal margin must be 24");

    const auto p4k = notes_layout_for_display_tier(DisplayTier::P4K);
    require(p4k.window_width == 1200, "4K notes width must be 1200");
    require(p4k.window_height == 1600, "4K notes height must be 1600");
    require(p4k.text_margin_horizontal == 36, "4K horizontal margin must be 36");
}

void test_monitor_resolution_selects_the_expected_notes_profile() {
    using realmheart::core::DisplayTier;
    using realmheart::ui::notes_layout_for_logical_geometry;

    require(
        notes_layout_for_logical_geometry(1920, 1080).display_tier == DisplayTier::P1080,
        "1080p monitor must select the 1080p notes profile"
    );
    require(
        notes_layout_for_logical_geometry(2560, 1440).display_tier == DisplayTier::P1440,
        "1440p monitor must select the 1440p notes profile"
    );
    require(
        notes_layout_for_logical_geometry(3840, 2160).display_tier == DisplayTier::P4K,
        "4K monitor must select the 4K notes profile"
    );
}

} // namespace

int main() {
    test_1080p_notes_baseline_is_preserved();
    test_higher_tiers_scale_notes_against_logical_viewport();
    test_monitor_resolution_selects_the_expected_notes_profile();
    std::cout << "Notes geometry tests passed\n";
    return 0;
}
