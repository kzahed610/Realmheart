#include "services/ProphecyMotionEngine.hpp"

#include <algorithm>

namespace realmheart::services {

void ProphecyMotionEngine::update(float elapsed_seconds, MotionState& out) {
    time_accum_ += elapsed_seconds;

    // Thread flow phase: slow, continuous loop (~10 seconds per cycle).
    out.thread_phase = std::fmod(time_accum_ * 0.1f, 1.0f);

    // Parallax: smooth interpolation toward target cursor position.
    // The background moves subtly opposite to cursor movement.
    const float parallax_strength = 0.08f;  // max 8% canvas shift
    current_parallax_x_ += (target_parallax_x_ - current_parallax_x_) * 0.05f;
    current_parallax_y_ += (target_parallax_y_ - current_parallax_y_) * 0.05f;

    // Center-relative offset: (0.5, 0.5) is center, so range is [-0.5, 0.5]
    out.parallax_x = (current_parallax_x_ - 0.5f) * parallax_strength * 2.0f;
    out.parallax_y = (current_parallax_y_ - 0.5f) * parallax_strength * 2.0f;

    // Sparkle: gentle sine wave modulation.
    out.particle_sparkle = (std::sin(time_accum_ * 2.5f) * 0.5f + 0.5f) * 0.3f + 0.1f;

    // Dominant pulse: if pulse is active, animate a scale bump.
    if (pulse_active_) {
        pulse_phase_ += elapsed_seconds * 8.0f;  // 8 Hz pulse
        if (pulse_phase_ >= 1.0f) {
            pulse_phase_ = 0.0f;
            pulse_active_ = false;
        }
        // Ease-out pulse: quick rise, slow fall.
        const float t = pulse_phase_;
        out.dominant_pulse = 1.0f + std::sin(t * M_PI) * 0.15f;
    } else {
        // Subtle idle breathing on dominant future.
        out.dominant_pulse = 1.0f + std::sin(time_accum_ * 0.8f) * 0.02f;
    }
}

void ProphecyMotionEngine::reset() {
    time_accum_ = 0.0f;
    current_parallax_x_ = 0.5f;
    current_parallax_y_ = 0.5f;
    target_parallax_x_ = 0.5f;
    target_parallax_y_ = 0.5f;
    pulse_phase_ = 0.0f;
    pulse_active_ = false;
}

void ProphecyMotionEngine::set_parallax_target(float x, float y) {
    target_parallax_x_ = std::clamp(x, 0.0f, 1.0f);
    target_parallax_y_ = std::clamp(y, 0.0f, 1.0f);
}

void ProphecyMotionEngine::trigger_dominant_pulse() {
    pulse_active_ = true;
    pulse_phase_ = 0.0f;
}

} // namespace realmheart::services
