#pragma once

#include <cstdint>

namespace realmheart::animation::character {

enum class CharacterExpressionPhase {
    Paused,
    HoldingBase,
    ClosingToInward,
    ClosedToInward,
    OpeningToInward,
    SettlingInward,
    HoldingInward,
    ClosingToBase,
    ClosedToBase,
    OpeningToBase,
    SettlingBase,
};

enum class CharacterEyeFrame {
    Base,
    Inward,
    Half,
    Closed,
};

enum class CharacterMouthFrame {
    Base,
    Curious,
};

struct CharacterExpressionSample {
    // Base means no overlay: the authored eyes/smile baked into base.png show
    // through unchanged.
    CharacterEyeFrame eyes = CharacterEyeFrame::Base;
    CharacterMouthFrame mouth = CharacterMouthFrame::Base;
};

// Pure, deterministic expression state machine. Texture selection lives in the
// compositor, while this component owns only timing and discrete overlay frames.
class CharacterExpressionAnimator {
public:
    static constexpr double kBlinkCloseSeconds = 0.110;
    static constexpr double kBlinkClosedSeconds = 0.120;
    static constexpr double kBlinkOpenSeconds = 0.130;
    static constexpr double kBaseReactionDelaySeconds = 0.300;
    static constexpr double kInwardReactionDelaySeconds = 0.220;

    CharacterExpressionAnimator();

    void reset_base();

    // Resumes from the last stable expression and begins a newly randomized
    // hold. Returns true only if the displayed overlay frame changed.
    bool resume();

    // Stops all expression timing and resolves any half/closed blink frame to
    // a stable expression so exits and rapid reversals cannot freeze an eyelid.
    bool pause_stable();

    // Advances by a monotonic delta. Returns true when eye or mouth overlay
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
    enum class StableExpression {
        Base,
        Inward,
    };

    void begin_hold(StableExpression expression);
    void begin_phase(CharacterExpressionPhase phase, double duration_seconds);
    void finish_phase();
    [[nodiscard]] double randomized_hold_seconds(StableExpression expression);
    [[nodiscard]] double random_unit() noexcept;
    [[nodiscard]] CharacterExpressionSample stable_sample(
        StableExpression expression
    ) const;
    [[nodiscard]] StableExpression stable_expression_for_phase() const noexcept;

    CharacterExpressionPhase phase_ = CharacterExpressionPhase::Paused;
    CharacterExpressionSample sample_{};
    StableExpression stable_expression_ = StableExpression::Base;
    double phase_elapsed_seconds_ = 0.0;
    double phase_duration_seconds_ = 0.0;
    std::uint32_t random_state_ = 0xA53C9E17u;
    unsigned int hold_count_ = 0;
};

} // namespace realmheart::animation::character
