#include "core/MonitorContext.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using realmheart::core::DisplayTier;
using realmheart::core::MonitorAspectClass;
using realmheart::core::monitor_context_for_geometry;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_ultrawide_keeps_height_owned_layout_tier() {
    const auto context = monitor_context_for_geometry(0, 0, 0, 3440, 1440, 1.0);
    require(context.layout_tier == DisplayTier::P1440,
            "3440x1440 must retain the 1440p layout tier");
    require(context.asset_tier == DisplayTier::P1440,
            "3440x1440 scale 1 must retain the 1440p asset tier");
    require(context.aspect == MonitorAspectClass::Ultrawide,
            "3440x1440 must classify as ultrawide");
}

void test_super_ultrawide_does_not_inflate_ui() {
    const auto context = monitor_context_for_geometry(1, 3440, 0, 5120, 1440, 1.0);
    require(context.layout_tier == DisplayTier::P1440,
            "5120x1440 must still use 1440p UI metrics");
    require(context.aspect == MonitorAspectClass::SuperUltrawide,
            "5120x1440 must classify as super-ultrawide");
}

void test_mixed_dpi_splits_layout_and_asset_tiers() {
    const auto context = monitor_context_for_geometry(2, 0, 0, 1920, 1080, 2.0);
    require(context.layout_tier == DisplayTier::P1080,
            "4K at scale 2 must keep 1080p logical layout metrics");
    require(context.asset_tier == DisplayTier::P4K,
            "4K at scale 2 must request 4K raster assets");
    require(context.physical_width_hint() == 3840 &&
                context.physical_height_hint() == 2160,
            "scale-2 physical hints must reconstruct the 4K backing size");
}

void test_fractional_scale_selects_crisp_assets_without_inflating_layout() {
    const auto context = monitor_context_for_geometry(4, 0, 0, 2560, 1440, 1.5);
    require(context.layout_tier == DisplayTier::P1440,
            "fractionally-scaled 4K must keep 1440p logical layout metrics");
    require(context.asset_tier == DisplayTier::P4K,
            "2560x1440 at scale 1.5 must request the 4K raster family");
    require(context.physical_width_hint() == 3840 &&
                context.physical_height_hint() == 2160,
            "fractional scale must preserve the physical backing-size hint");
}

void test_portrait_classification_is_independent_of_density() {
    const auto context = monitor_context_for_geometry(3, -1080, 0, 1080, 1920, 1.0);
    require(context.aspect == MonitorAspectClass::Portrait,
            "1080x1920 must classify as portrait");
    require(context.layout_tier == DisplayTier::P1080,
            "rotated 1080p must preserve the 1080p layout tier");
}

} // namespace

int main() {
    test_ultrawide_keeps_height_owned_layout_tier();
    test_super_ultrawide_does_not_inflate_ui();
    test_mixed_dpi_splits_layout_and_asset_tiers();
    test_fractional_scale_selects_crisp_assets_without_inflating_layout();
    test_portrait_classification_is_independent_of_density();
    std::cout << "Monitor context tests passed\n";
    return 0;
}
