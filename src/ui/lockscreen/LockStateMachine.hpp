#pragma once

namespace realmheart::ui::lockscreen {

// Phase machine for the Broken Seal lockscreen.
enum class LockPhase {
    Hidden,
    Opening,
    Splitting,
    Typing,
    Snapped,
    Combining,
    Closing,
};

// Tracks a 0..1 timeline for the currently animating phase and derives eased
// outputs for the scene. Non-animating phases return progress() == 0.
class LockStateMachine {
public:
    // Opens the seal: Hidden -> Opening. Re-entrant (Opening during Closing
    // restarts the emergence).
    void present() noexcept;

    // Opens the seal already fully composed (mana-core style): skips the
    // emergence and lands directly in Typing. Closing still plays on dismiss.
    void present_instant() noexcept;

    // Dismisses the seal: any active phase -> Closing.
    void dismiss() noexcept;

    // Immediately returns to Hidden without animation.
    void hide_immediately() noexcept;

    // Advances the active phase by delta_seconds. Non-finite deltas and deltas
    // above one second are ignored/clamped so a stall never causes a jump.
    void advance(double delta_seconds) noexcept;

    [[nodiscard]] LockPhase phase() const noexcept { return phase_; }
    [[nodiscard]] double progress() const noexcept { return progress_; }

    // True while a phase needs continuous frames.
    [[nodiscard]] bool needs_frame() const noexcept;

private:
    LockPhase phase_ = LockPhase::Hidden;
    double progress_ = 0.0;
};

} // namespace realmheart::ui::lockscreen
