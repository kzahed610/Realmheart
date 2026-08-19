#pragma once

#include <cmath>
#include <cstdint>

namespace realmheart::services {

// Prophecy motion system: drives animated threads/runes between futures
// and parallax depth on the background. Pure logic (no GL) — the renderer
// queries state and applies uniforms.
class ProphecyMotionEngine {
public:
    // Visual state that maps to the renderer's animation uniforms.
    struct MotionState {
        float thread_phase = 0.0f;       // 0..1 animation phase for thread flow
        float parallax_x = 0.0f;          // background parallax offset (-1..1)
        float parallax_y = 0.0f;          // background parallax offset (-1..1)
        float particle_sparkle = 0.0f;    // global sparkle intensity (0..1)
        float dominant_pulse = 0.0f;      // dominant future scale pulse (1.0 = normal)
    };

    ProphecyMotionEngine() = default;

    // Advance the animation by elapsed_seconds. Produces the current MotionState.
    void update(float elapsed_seconds, MotionState& out);

    // Reset to initial state (called when lock screen first appears).
    void reset();

    // Set target cursor position (normalized 0..1) for parallax.
    // Call this from the renderer's pointer motion handler.
    void set_parallax_target(float x, float y);

    // Pulse the dominant future (called when user interacts with password field).
    void trigger_dominant_pulse();

private:
    float time_accum_ = 0.0f;
    float target_parallax_x_ = 0.5f;
    float target_parallax_y_ = 0.5f;
    float current_parallax_x_ = 0.5f;
    float current_parallax_y_ = 0.5f;
    float pulse_phase_ = 0.0f;
    bool pulse_active_ = false;
};

} // namespace realmheart::services
