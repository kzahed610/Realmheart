#include "services/ProphecyMotionEngine.hpp"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

void require_approx(float a, float b, float eps, const char* msg) {
    if (std::fabs(a - b) > eps) {
        std::cerr << "FAIL: " << msg << " (got " << a << ", expected ~" << b << ")\n";
        std::exit(1);
    }
}

void test_reset_produces_clean_state() {
    realmheart::services::ProphecyMotionEngine engine;
    engine.set_parallax_target(0.9f, 0.1f);
    engine.trigger_dominant_pulse();

    realmheart::services::ProphecyMotionEngine::MotionState state;
    engine.update(0.016f, state);

    // Reset should center parallax.
    engine.reset();

    realmheart::services::ProphecyMotionEngine::MotionState state2;
    engine.update(0.001f, state2);
    // After reset, parallax should be centered (0.0).
    require_approx(state2.parallax_x, 0.0f, 0.001f, "parallax_x should be ~0 after reset");
    require_approx(state2.parallax_y, 0.0f, 0.001f, "parallax_y should be ~0 after reset");
}

void test_thread_phase_advances() {
    realmheart::services::ProphecyMotionEngine engine;
    realmheart::services::ProphecyMotionEngine::MotionState state;

    engine.update(1.0f, state);
    float phase1 = state.thread_phase;

    engine.update(1.0f, state);
    float phase2 = state.thread_phase;

    // Phase should advance by 0.1 per second.
    require_approx(phase2, phase1 + 0.1f, 0.001f, "thread phase should advance by 0.1 per second");
}

void test_parallax_smooth_interpolation() {
    realmheart::services::ProphecyMotionEngine engine;
    engine.reset();  // parallax at center (0.5, 0.5)

    // Set extreme target.
    engine.set_parallax_target(1.0f, 1.0f);

    // After one frame, should not have jumped to full parallax.
    realmheart::services::ProphecyMotionEngine::MotionState state;
    engine.update(0.016f, state);

    // Parallax should be small (interpolating toward target, not there yet).
    require(std::fabs(state.parallax_x) < 0.08f, "parallax should not jump immediately to max");
    require(std::fabs(state.parallax_y) < 0.08f, "parallax y should not jump immediately to max");

    // After many frames, should approach max.
    for (int i = 0; i < 100; ++i) {
        engine.update(0.016f, state);
    }
    require_approx(state.parallax_x, 0.08f, 0.001f, "parallax_x should approach max after many frames");
    require_approx(state.parallax_y, 0.08f, 0.001f, "parallax_y should approach max after many frames");
}

void test_parallax_clamps_to_range() {
    realmheart::services::ProphecyMotionEngine engine;
    engine.reset();

    // Set targets way outside [0,1] — should be clamped.
    engine.set_parallax_target(5.0f, -3.0f);

    realmheart::services::ProphecyMotionEngine::MotionState state;
    for (int i = 0; i < 200; ++i) {
        engine.update(0.016f, state);
    }

    require_approx(state.parallax_x, 0.08f, 0.001f, "parallax_x must clamp to max");
    require_approx(state.parallax_y, -0.08f, 0.001f, "parallax_y must clamp to min");
}

void test_pulse_triggers_scale_bump() {
    realmheart::services::ProphecyMotionEngine engine;
    engine.reset();

    realmheart::services::ProphecyMotionEngine::MotionState state;
    engine.update(0.016f, state);
    float idle_pulse = state.dominant_pulse;

    engine.trigger_dominant_pulse();

    // After triggering, pulse should start rising above idle.
    engine.update(0.016f, state);
    require(state.dominant_pulse > idle_pulse, "pulse should rise above idle after trigger");

    // Wait for pulse to complete (8 Hz, so ~0.125s).
    for (int i = 0; i < 30; ++i) {
        engine.update(0.016f, state);
    }

    // After pulse completes, should return to idle breathing.
    require_approx(state.dominant_pulse, 1.0f, 0.05f, "pulse should return to ~1.0 after completion");
}

void test_pulse_reaches_1p15_peak() {
    realmheart::services::ProphecyMotionEngine engine;
    engine.reset();
    engine.trigger_dominant_pulse();

    realmheart::services::ProphecyMotionEngine::MotionState state;
    float max_pulse = 0.0f;

    // Animate through the full pulse cycle.
    for (int i = 0; i < 20; ++i) {
        engine.update(0.016f, state);
        max_pulse = std::max(max_pulse, state.dominant_pulse);
    }

    // Peak should be about 1.15 (1.0 + 0.15 * sin(pi/2)).
    require(max_pulse > 1.1f, "pulse peak should exceed 1.1");
    require(max_pulse < 1.2f, "pulse peak should not exceed 1.2");
}

void test_sparkle_in_valid_range() {
    realmheart::services::ProphecyMotionEngine engine;
    engine.reset();

    realmheart::services::ProphecyMotionEngine::MotionState state;
    for (int i = 0; i < 100; ++i) {
        engine.update(0.016f, state);
        require(state.particle_sparkle >= 0.0f, "sparkle must be non-negative");
        require(state.particle_sparkle <= 1.0f, "sparkle must be <= 1.0");
    }
}

void test_continuous_update_no_crash() {
    realmheart::services::ProphecyMotionEngine engine;
    engine.reset();

    // Simulate 10 seconds of animation at 60fps.
    realmheart::services::ProphecyMotionEngine::MotionState state;
    for (int i = 0; i < 600; ++i) {
        engine.update(0.016f, state);
        require(state.thread_phase >= 0.0f && state.thread_phase < 1.0f, "thread_phase must wrap to [0,1)");
    }
}

void test_thread_phase_wraps_at_1() {
    realmheart::services::ProphecyMotionEngine engine;
    engine.reset();

    realmheart::services::ProphecyMotionEngine::MotionState state;

    // 10 seconds: 10 * 0.1 = 1.0 phase → wraps to 0.0.
    engine.update(10.0f, state);
    require_approx(state.thread_phase, 0.0f, 0.001f, "thread_phase should wrap to 0 after 10 seconds");
}

} // namespace

int main() {
    test_reset_produces_clean_state();
    test_thread_phase_advances();
    test_parallax_smooth_interpolation();
    test_parallax_clamps_to_range();
    test_pulse_triggers_scale_bump();
    test_pulse_reaches_1p15_peak();
    test_sparkle_in_valid_range();
    test_continuous_update_no_crash();
    test_thread_phase_wraps_at_1();

    std::cout << "All ProphecyMotionEngine tests PASSED\n";
    return 0;
}
