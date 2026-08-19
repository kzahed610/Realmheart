#include "services/ProphecyLayoutEngine.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

void test_empty_futures_below_minimum() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 3);
    require(layout.futures.empty(), "below minimum futures count must return empty layout");
}

void test_empty_futures_above_maximum() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 7);
    require(layout.futures.empty(), "above maximum futures count must return empty layout");
}

void test_four_futures_has_five_entries() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 4);
    require(layout.futures.size() == 4, "must have 4 futures (3 + dominant)");
    require(layout.futures[0].is_dominant, "first future must be dominant");
    require(layout.futures[0].is_active, "dominant must be active");
    require(!layout.futures[1].is_dominant, "second future must not be dominant");
    require(!layout.futures[2].is_dominant, "third future must not be dominant");
    require(!layout.futures[3].is_dominant, "fourth future must not be dominant");
}

void test_five_futures_has_five_entries() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 5);
    require(layout.futures.size() == 5, "must have 5 futures");
}

void test_six_futures_has_six_entries() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 6);
    require(layout.futures.size() == 6, "must have 6 futures");
}

void test_deterministic_same_seed_same_output() {
    auto layout1 = realmheart::services::ProphecyLayoutEngine::compute(12345, 6);
    auto layout2 = realmheart::services::ProphecyLayoutEngine::compute(12345, 6);
    require(layout1.futures.size() == layout2.futures.size(), "same seed must produce same futures count");

    for (std::size_t i = 0; i < layout1.futures.size(); ++i) {
        const auto& f1 = layout1.futures[i];
        const auto& f2 = layout2.futures[i];
        require(f1.x == f2.x, "x must be deterministic");
        require(f1.y == f2.y, "y must be deterministic");
        require(f1.width == f2.width, "width must be deterministic");
        require(f1.height == f2.height, "height must be deterministic");
    }
}

void test_different_seed_produces_different_jitter() {
    auto layout1 = realmheart::services::ProphecyLayoutEngine::compute(1, 6);
    auto layout2 = realmheart::services::ProphecyLayoutEngine::compute(2, 6);

    // Jitter should differ for at least one future.
    bool differs = false;
    for (std::size_t i = 0; i < layout1.futures.size(); ++i) {
        if (layout1.futures[i].x != layout2.futures[i].x ||
            layout1.futures[i].y != layout2.futures[i].y) {
            differs = true;
            break;
        }
    }
    require(differs, "different seeds must produce different jitter");
}

void test_coordinates_within_bounds() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 6);
    for (const auto& f : layout.futures) {
        require(f.x >= 0.0 && f.x <= 1.0, "x must be within [0,1]");
        require(f.y >= 0.0 && f.y <= 1.0, "y must be within [0,1]");
        require(f.x + f.width <= 1.0 + 1e-9, "futures must not exceed right edge");
        require(f.y + f.height <= 1.0 + 1e-9, "futures must not exceed bottom edge");
        require(f.width > 0.0 && f.width < 1.0, "width must be positive and less than full canvas");
        require(f.height > 0.0 && f.height < 1.0, "height must be positive and less than full canvas");
    }
}

void test_dominant_future_at_center() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 6);
    const auto& dominant = layout.futures[0];
    require(dominant.is_dominant, "first future must be dominant");
    // Dominant should be centered-ish.
    require(dominant.x > 0.2 && dominant.x < 0.5, "dominant x should be near center");
    require(dominant.y > 0.2 && dominant.y < 0.6, "dominant y should be near center");
}

void test_rinia_anchor_present() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 4);
    require(layout.rinia_width > 0.0, "rinia width must be positive");
    require(layout.rinia_height > 0.0, "rinia height must be positive");
    require(layout.rinia_x > 0.0, "rinia x must be positive");
    require(layout.rinia_y > 0.0, "rinia y must be positive");
}

void test_protected_regions_present() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 4);
    require(layout.protected_regions[0].width > 0.0, "first protected region (Rinia) must have positive width");
    require(layout.protected_regions[1].width > 0.0, "second protected region (clock) must have positive width");
    require(layout.protected_regions[2].width > 0.0, "third protected region (password) must have positive width");
}

void test_canvas_dimensions_default() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 4);
    require(layout.canvas_width == 1920, "default canvas width must be 1920");
    require(layout.canvas_height == 1080, "default canvas height must be 1080");
}

void test_canvas_dimensions_custom() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 4, 2560, 1440);
    require(layout.canvas_width == 2560, "custom canvas width must be 2560");
    require(layout.canvas_height == 1440, "custom canvas height must be 1440");
}

void test_seed_preserved() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(99999, 4);
    require(layout.seed == 99999, "seed must be preserved in layout");
}

void test_jitter_stays_within_slot_bounds() {
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(42, 6);
    // For 6 futures, check that jitter doesn't push futures outside their slot area.
    // The jitter is ±3% of canvas, so futures should still be in roughly the right quadrant.
    for (std::size_t i = 1; i < layout.futures.size(); ++i) {  // skip dominant
        const auto& f = layout.futures[i];
        // Each non-dominant future should be in one of the grid quadrants.
        require(f.x < 0.95, "non-dominant future x must be within reasonable bounds");
        require(f.y < 0.95, "non-dominant future y must be within reasonable bounds");
    }
}

} // namespace

int main() {
    test_empty_futures_below_minimum();
    test_empty_futures_above_maximum();
    test_four_futures_has_five_entries();
    test_five_futures_has_five_entries();
    test_six_futures_has_six_entries();
    test_deterministic_same_seed_same_output();
    test_different_seed_produces_different_jitter();
    test_coordinates_within_bounds();
    test_dominant_future_at_center();
    test_rinia_anchor_present();
    test_protected_regions_present();
    test_canvas_dimensions_default();
    test_canvas_dimensions_custom();
    test_seed_preserved();
    test_jitter_stays_within_slot_bounds();

    std::cout << "All ProphecyLayoutEngine tests PASSED\n";
    return 0;
}
