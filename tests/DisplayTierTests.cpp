#include "core/DisplayTier.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using realmheart::core::DisplayTier;
using realmheart::core::display_tier_for_logical_geometry;
using realmheart::core::display_tier_spec;
using realmheart::core::scale_dimension;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_supported_logical_resolutions_select_explicit_tiers() {
    require(
        display_tier_for_logical_geometry(1920, 1080) == DisplayTier::P1080,
        "1920x1080 must select the 1080p tier"
    );
    require(
        display_tier_for_logical_geometry(2560, 1440) == DisplayTier::P1440,
        "2560x1440 must select the 1440p tier"
    );
    require(
        display_tier_for_logical_geometry(3840, 2160) == DisplayTier::P4K,
        "3840x2160 must select the 4K tier"
    );
}

void test_invalid_geometry_falls_back_to_1080p() {
    require(
        display_tier_for_logical_geometry(0, 0) == DisplayTier::P1080,
        "missing monitor geometry must fall back to 1080p"
    );
    require(
        display_tier_for_logical_geometry(-1, 1080) == DisplayTier::P1080,
        "invalid monitor geometry must fall back to 1080p"
    );
}

void test_specs_expose_stable_runtime_directory_and_scale() {
    const auto hd = display_tier_spec(DisplayTier::P1080);
    require(hd.directory == "1080p", "1080p directory token must be stable");
    require(hd.logical_width == 1920 && hd.logical_height == 1080,
            "1080p logical dimensions must be stable");
    require(std::abs(hd.scale - 1.0) < 0.000001,
            "1080p scale must be one");

    const auto qhd = display_tier_spec(DisplayTier::P1440);
    require(qhd.directory == "1440p", "1440p directory token must be stable");
    require(qhd.logical_width == 2560 && qhd.logical_height == 1440,
            "1440p logical dimensions must be stable");
    require(std::abs(qhd.scale - (4.0 / 3.0)) < 0.000001,
            "1440p scale must be four thirds");

    const auto uhd = display_tier_spec(DisplayTier::P4K);
    require(uhd.directory == "4k", "4K directory token must be stable");
    require(uhd.logical_width == 3840 && uhd.logical_height == 2160,
            "4K logical dimensions must be stable");
    require(std::abs(uhd.scale - 2.0) < 0.000001,
            "4K scale must be two");
}

void test_raster_dimension_rounding_is_deterministic() {
    require(scale_dimension(548, 1.0) == 548,
            "1080p dimensions must remain unchanged");
    require(scale_dimension(548, 4.0 / 3.0) == 731,
            "1440p dimensions must use round-half-away-from-zero");
    require(scale_dimension(548, 2.0) == 1096,
            "4K dimensions must double exactly");
}

} // namespace

int main() {
    test_supported_logical_resolutions_select_explicit_tiers();
    test_invalid_geometry_falls_back_to_1080p();
    test_specs_expose_stable_runtime_directory_and_scale();
    test_raster_dimension_rounding_is_deterministic();
    std::cout << "Display tier tests passed\n";
    return 0;
}
