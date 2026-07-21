#pragma once

namespace realmheart::animation::character {

enum class CharacterAnimationPhase {
    Hidden,
    Entering,
    Settling,
    Idle,
    Exiting,
};

struct CharacterAnimationSample {
    double visibility = 0.0;
    double offset_x = 0.0;
    double offset_y = 0.0;

    // Relative displacement applied only to mask-weighted hair tips. Roots
    // still follow offset_x exactly, while lower hair lags behind the body.
    double hair_tip_offset_x = 0.0;
};

// Pure timing/state component. It owns no GTK objects and performs no drawing,
// which keeps lifecycle logic testable and reusable by future character rigs.
class CharacterAnimator {
public:
    static constexpr double kEnterDurationSeconds = 0.28;
    static constexpr double kSettleDurationSeconds = 0.18;
    static constexpr double kExitDurationSeconds = 0.28;

    CharacterAnimator();

    void snap_hidden();
    void snap_idle();
    void start_enter();
    void start_exit();

    // Advances by a monotonic delta. Returns true when the visible sample or
    // phase changed and the compositor should redraw.
    bool advance(double delta_seconds);

    [[nodiscard]] CharacterAnimationPhase phase() const noexcept { return phase_; }
    [[nodiscard]] const CharacterAnimationSample& sample() const noexcept {
        return sample_;
    }
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] double display_opacity() const noexcept;
    [[nodiscard]] static double idle_hair_wave(
        double elapsed_seconds,
        double phase_radians
    ) noexcept;
    [[nodiscard]] bool hidden() const noexcept {
        return phase_ == CharacterAnimationPhase::Hidden;
    }
    [[nodiscard]] bool idle() const noexcept {
        return phase_ == CharacterAnimationPhase::Idle;
    }

private:
    void begin_transition(
        CharacterAnimationPhase phase,
        double duration_seconds,
        CharacterAnimationSample target
    );
    void finish_transition();
    static double ease_out_cubic(double progress);
    static double ease_in_cubic(double progress);
    static CharacterAnimationSample interpolate(
        const CharacterAnimationSample& from,
        const CharacterAnimationSample& to,
        double progress
    );

    CharacterAnimationPhase phase_ = CharacterAnimationPhase::Hidden;
    CharacterAnimationSample sample_{};
    CharacterAnimationSample transition_start_{};
    CharacterAnimationSample transition_target_{};
    double transition_elapsed_seconds_ = 0.0;
    double transition_duration_seconds_ = 0.0;
};

} // namespace realmheart::animation::character
