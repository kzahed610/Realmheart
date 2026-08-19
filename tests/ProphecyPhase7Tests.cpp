#include "services/ProphecyLayoutEngine.hpp"
#include "services/ProphecyMotionEngine.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

namespace {

void require(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

// Phase 7: Performance and abuse testing for the Prophecy lock screen.

// Test 1: Layout engine must compute in <1ms for 6-future layout.
void test_layout_performance() {
    using namespace std::chrono;
    auto start = high_resolution_clock::now();
    for (int i = 0; i < 10000; ++i) {
        volatile auto layout = realmheart::services::ProphecyLayoutEngine::compute(
            static_cast<std::uint64_t>(i), 6, 1920, 1080
        );
        (void)layout;
    }
    auto elapsed = duration_cast<microseconds>(high_resolution_clock::now() - start);
    double avg_us = elapsed.count() / 10000.0;
    std::cout << "  Layout avg: " << avg_us << " us\n";
    require(avg_us < 100.0, "layout must be <100us avg");
}

// Test 2: Motion engine must update in <0.5ms per frame.
void test_motion_performance() {
    using namespace std::chrono;
    realmheart::services::ProphecyMotionEngine engine;
    engine.reset();
    auto start = high_resolution_clock::now();
    for (int i = 0; i < 100000; ++i) {
        realmheart::services::ProphecyMotionEngine::MotionState state;
        engine.update(0.016f, state);
    }
    auto elapsed = duration_cast<microseconds>(high_resolution_clock::now() - start);
    double avg_us = elapsed.count() / 100000.0;
    std::cout << "  Motion avg: " << avg_us << " us\n";
    require(avg_us < 50.0, "motion update must be <50us avg");
}

// Test 3: Deterministic across same seed.
void test_deterministic_seed() {
    auto l1 = realmheart::services::ProphecyLayoutEngine::compute(42, 6, 1920, 1080);
    auto l2 = realmheart::services::ProphecyLayoutEngine::compute(42, 6, 1920, 1080);
    require(l1.futures.size() == l2.futures.size(), "same seed must produce same count");
    for (size_t i = 0; i < l1.futures.size(); ++i) {
        require(l1.futures[i].x == l2.futures[i].x, "x must be deterministic");
        require(l1.futures[i].y == l2.futures[i].y, "y must be deterministic");
    }
}

// Test 4: Abuse test — pathological seeds don't crash or produce NaN.
void test_pathological_seeds() {
    std::vector<std::uint64_t> bad_seeds = {
        0, 1, UINT64_MAX, UINT64_MAX - 1,
        0xDEADBEEF, 0xFFFFFFFFFFFFFFFF,
        static_cast<std::uint64_t>(-1),
    };
    for (auto seed : bad_seeds) {
        auto layout = realmheart::services::ProphecyLayoutEngine::compute(seed, 6, 1920, 1080);
        require(layout.futures.size() > 0, "pathological seed must still produce layout");
        for (const auto& f : layout.futures) {
            require(f.x >= 0.0f && f.x <= 1.0f, "x must be in [0,1]");
            require(f.y >= 0.0f && f.y <= 1.0f, "y must be in [0,1]");
            // Check for NaN/inf
            require(f.x == f.x, "x must not be NaN");
            require(f.y == f.y, "y must not be NaN");
        }
    }
}

// Test 5: Motion engine stays bounded under long simulation.
void test_motion_bounds_long() {
    realmheart::services::ProphecyMotionEngine engine;
    engine.reset();
    for (int i = 0; i < 100000; ++i) {
        realmheart::services::ProphecyMotionEngine::MotionState state;
        engine.update(0.016f, state);
        require(state.parallax_x >= -1.0f && state.parallax_x <= 1.0f, "parallax x bounded");
        require(state.parallax_y >= -1.0f && state.parallax_y <= 1.0f, "parallax y bounded");
        require(state.dominant_pulse >= 0.0f && state.dominant_pulse <= 2.0f, "pulse bounded");
    }
}

// Test 6: Layout respects MIN/MAX FUTURES bounds.
void test_future_bounds() {
    auto min_layout = realmheart::services::ProphecyLayoutEngine::compute(1, 4, 1920, 1080);
    auto max_layout = realmheart::services::ProphecyLayoutEngine::compute(1, 6, 1920, 1080);
    require(min_layout.futures.size() == 4, "MIN_FUTURES=4");
    require(max_layout.futures.size() == 6, "MAX_FUTURES=6");
}

} // namespace

int main() {
    std::cout << "Phase 7: Performance and abuse tests\n";
    test_layout_performance();
    test_motion_performance();
    test_deterministic_seed();
    test_pathological_seeds();
    test_motion_bounds_long();
    test_future_bounds();
    std::cout << "All Phase 7 tests PASSED\n";
    return 0;
}
