#pragma once

#include <cstdint>

namespace realmheart::animation::character {

enum class CharacterExpressionPhase {
    Paused,
    HoldingInward,
    ClosingToUser,
    ClosedToUser,
    OpeningToUser,
    SettlingUser,
    HoldingUser,
    ClosingToInward,
    ClosedToInward,
    OpeningToInward,
    SettlingInward,
};

enum class CharacterEyeFrame {
    Inward,
    User,
    Half,
    Closed,
};

enum class CharacterMouthFrame {
    Curious,
    Smile,
};

struct CharacterExpressionSample {
    CharacterEyeFrame eyes = CharacterEyeFrame::Inward;
    CharacterMouthFrame mouth = CharacterMouthFrame::Curious;
};

// Pure, deterministic expression state machine. Texture selection lives in the
// compositor, while this component owns only timing and discrete face frames.
class CharacterExpressionAnimator {
public:
    static constexpr double kBlinkCloseSeconds = 0.110;
    static constexpr double kBlinkClosedSeconds = 0.120;
    static constexpr double kBlinkOpenSeconds = 0.130;
    static constexpr double kUserReactionDelaySeconds = 0.300;
    static constexpr double kInwardReactionDelaySeconds = 0.220;

    CharacterExpressionAnimator();

    void reset_inward();

    // Resumes from the last stable gaze and begins a newly randomized hold.
    // Returns true only if the displayed face frame changed.
    bool resume();

    // Stops all expression timing and resolves any half/closed blink frame to
    // a stable gaze so exits and rapid reversals cannot freeze an eyelid frame.
    bool pause_stable();

    // Advances by a monotonic delta. Returns true when eye or mouth texture
    // selection changed and the expression plane should redraw.
    bool advance(double delta_seconds);

    [[nodiscard]] CharacterExpressionPhase phase() const noexcept {
        return phase_;
    }
    [[nodiscard]] const CharacterExpressionSample& sample() const noexcept {
        return sample_;
    }
    [[nodiscard]] bool active() const noexcept {
        return phase_ != CharacterExpressionPhase::Paused;
    }

private:
    enum class StableGaze {
        Inward,
        User,
    };

    void begin_hold(StableGaze gaze);
    void begin_phase(CharacterExpressionPhase phase, double duration_seconds);
    void finish_phase();
    [[nodiscard]] double randomized_hold_seconds(StableGaze gaze);
    [[nodiscard]] double random_unit() noexcept;
    [[nodiscard]] CharacterExpressionSample stable_sample(StableGaze gaze) const;
    [[nodiscard]] StableGaze stable_gaze_for_phase() const noexcept;

    CharacterExpressionPhase phase_ = CharacterExpressionPhase::Paused;
    CharacterExpressionSample sample_{};
    StableGaze stable_gaze_ = StableGaze::Inward;
    double phase_elapsed_seconds_ = 0.0;
    double phase_duration_seconds_ = 0.0;
    std::uint32_t random_state_ = 0xA53C9E17u;
    unsigned int hold_count_ = 0;
};

} // namespace realmheart::animation::character
