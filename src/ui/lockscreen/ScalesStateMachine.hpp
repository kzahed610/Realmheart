#pragma once

namespace realmheart::ui::lockscreen {

// Phase machine for the Broken Seal scales lockscreen.
enum class ScalesPhase {
    Hidden,
    Forming,
    Idle,
    Failing,
    Closing,
};

// Tracks a 0..1 timeline for the currently animating phase and derives eased
// outputs for the scene. Non-animating phases return progress() == 0.
class ScalesStateMachine {
public:
    // Hidden -> Forming. Re-entrant (Forming during Closing restarts the
    // emergence).
    void present() noexcept;

    // Any active phase -> Closing (unlock/dismiss). Idle is skipped.
    void dismiss() noexcept;

    // Forming/Idle -> Failing (wrong password). Clears after the flash.
    void fail() noexcept;

    // Immediately returns to Hidden without animation.
    void hide_immediately() noexcept;

    // Advances the active phase by delta_seconds. Non-finite deltas and deltas
    // above one second are ignored/clamped so a stall never causes a jump.
    void advance(double delta_seconds) noexcept;

    [[nodiscard]] ScalesPhase phase() const noexcept { return phase_; }
    [[nodiscard]] double progress() const noexcept { return progress_; }

    // True while a phase needs continuous frames.
    [[nodiscard]] bool needs_frame() const noexcept;

private:
    ScalesPhase phase_ = ScalesPhase::Hidden;
    double progress_ = 0.0;
};

} // namespace realmheart::ui::lockscreen
