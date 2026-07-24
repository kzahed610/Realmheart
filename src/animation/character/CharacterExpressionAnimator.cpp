#include "animation/character/CharacterExpressionAnimator.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::animation::character {
namespace {

bool samples_differ(
    const CharacterExpressionSample& lhs,
    const CharacterExpressionSample& rhs
) {
    return lhs.eyes != rhs.eyes || lhs.mouth != rhs.mouth;
}

} // namespace

CharacterExpressionAnimator::CharacterExpressionAnimator() {
    reset_base();
}

void CharacterExpressionAnimator::reset_base() {
    phase_ = CharacterExpressionPhase::Paused;
    stable_expression_ = StableExpression::Base;
    sample_ = stable_sample(stable_expression_);
    phase_elapsed_seconds_ = 0.0;
    phase_duration_seconds_ = 0.0;
}

bool CharacterExpressionAnimator::resume() {
    if (active()) return false;

    const CharacterExpressionSample before = sample_;
    begin_hold(stable_expression_);
    return samples_differ(before, sample_);
}

bool CharacterExpressionAnimator::pause_stable() {
    const CharacterExpressionSample before = sample_;
    stable_expression_ = stable_expression_for_phase();
    sample_ = stable_sample(stable_expression_);
    phase_ = CharacterExpressionPhase::Paused;
    phase_elapsed_seconds_ = 0.0;
    phase_duration_seconds_ = 0.0;
    return samples_differ(before, sample_);
}

bool CharacterExpressionAnimator::advance(double delta_seconds) {
    if (!active()) return false;

    const CharacterExpressionSample before = sample_;
    double remaining = std::clamp(delta_seconds, 0.0, 0.50);

    while (remaining > 0.0 && active()) {
        const double duration = std::max(phase_duration_seconds_, 0.000001);
        const double available = std::max(duration - phase_elapsed_seconds_, 0.0);
        const double consumed = std::min(remaining, available);
        phase_elapsed_seconds_ += consumed;
        remaining -= consumed;

        if (phase_elapsed_seconds_ >= duration) finish_phase();
        if (consumed <= 0.0) break;
    }

    return samples_differ(before, sample_);
}

void CharacterExpressionAnimator::begin_hold(StableExpression expression) {
    stable_expression_ = expression;
    begin_phase(
        expression == StableExpression::Base
            ? CharacterExpressionPhase::HoldingBase
            : CharacterExpressionPhase::HoldingInward,
        randomized_hold_seconds(expression)
    );
}

void CharacterExpressionAnimator::begin_phase(
    CharacterExpressionPhase phase,
    double duration_seconds
) {
    phase_ = phase;
    phase_elapsed_seconds_ = 0.0;
    phase_duration_seconds_ = duration_seconds;

    switch (phase_) {
    case CharacterExpressionPhase::Paused:
        sample_ = stable_sample(stable_expression_);
        break;
    case CharacterExpressionPhase::HoldingBase:
        stable_expression_ = StableExpression::Base;
        sample_ = stable_sample(stable_expression_);
        break;
    case CharacterExpressionPhase::ClosingToInward:
        sample_ = {
            .eyes = CharacterEyeFrame::Half,
            .mouth = CharacterMouthFrame::Base,
        };
        break;
    case CharacterExpressionPhase::ClosedToInward:
        stable_expression_ = StableExpression::Inward;
        sample_ = {
            .eyes = CharacterEyeFrame::Closed,
            .mouth = CharacterMouthFrame::Curious,
        };
        break;
    case CharacterExpressionPhase::OpeningToInward:
        sample_ = {
            .eyes = CharacterEyeFrame::Half,
            .mouth = CharacterMouthFrame::Curious,
        };
        break;
    case CharacterExpressionPhase::SettlingInward:
        sample_ = stable_sample(StableExpression::Inward);
        break;
    case CharacterExpressionPhase::HoldingInward:
        stable_expression_ = StableExpression::Inward;
        sample_ = stable_sample(stable_expression_);
        break;
    case CharacterExpressionPhase::ClosingToBase:
        sample_ = {
            .eyes = CharacterEyeFrame::Half,
            .mouth = CharacterMouthFrame::Curious,
        };
        break;
    case CharacterExpressionPhase::ClosedToBase:
        stable_expression_ = StableExpression::Base;
        sample_ = {
            .eyes = CharacterEyeFrame::Closed,
            .mouth = CharacterMouthFrame::Base,
        };
        break;
    case CharacterExpressionPhase::OpeningToBase:
        sample_ = {
            .eyes = CharacterEyeFrame::Half,
            .mouth = CharacterMouthFrame::Base,
        };
        break;
    case CharacterExpressionPhase::SettlingBase:
        sample_ = stable_sample(StableExpression::Base);
        break;
    }
}

void CharacterExpressionAnimator::finish_phase() {
    switch (phase_) {
    case CharacterExpressionPhase::Paused:
        break;
    case CharacterExpressionPhase::HoldingBase:
        begin_phase(
            CharacterExpressionPhase::ClosingToInward,
            kBlinkCloseSeconds
        );
        break;
    case CharacterExpressionPhase::ClosingToInward:
        begin_phase(
            CharacterExpressionPhase::ClosedToInward,
            kBlinkClosedSeconds
        );
        break;
    case CharacterExpressionPhase::ClosedToInward:
        begin_phase(
            CharacterExpressionPhase::OpeningToInward,
            kBlinkOpenSeconds
        );
        break;
    case CharacterExpressionPhase::OpeningToInward:
        begin_phase(
            CharacterExpressionPhase::SettlingInward,
            kInwardReactionDelaySeconds
        );
        break;
    case CharacterExpressionPhase::SettlingInward:
        begin_hold(StableExpression::Inward);
        break;
    case CharacterExpressionPhase::HoldingInward:
        begin_phase(
            CharacterExpressionPhase::ClosingToBase,
            kBlinkCloseSeconds
        );
        break;
    case CharacterExpressionPhase::ClosingToBase:
        begin_phase(
            CharacterExpressionPhase::ClosedToBase,
            kBlinkClosedSeconds
        );
        break;
    case CharacterExpressionPhase::ClosedToBase:
        begin_phase(
            CharacterExpressionPhase::OpeningToBase,
            kBlinkOpenSeconds
        );
        break;
    case CharacterExpressionPhase::OpeningToBase:
        begin_phase(
            CharacterExpressionPhase::SettlingBase,
            kBaseReactionDelaySeconds
        );
        break;
    case CharacterExpressionPhase::SettlingBase:
        begin_hold(StableExpression::Base);
        break;
    }
}

double CharacterExpressionAnimator::randomized_hold_seconds(
    StableExpression expression
) {
    const double minimum = expression == StableExpression::Inward ? 3.2 : 2.6;
    const double maximum = expression == StableExpression::Inward ? 5.0 : 4.0;
    double duration = minimum + ((maximum - minimum) * random_unit());

    // Roughly one hold in five lingers a little longer. It remains bounded and
    // deterministic, avoiding both a mechanical GIF loop and untestable RNG.
    ++hold_count_;
    if ((hold_count_ % 5u) == 0u) {
        duration += 0.8 + (1.2 * random_unit());
    }
    return duration;
}

double CharacterExpressionAnimator::random_unit() noexcept {
    std::uint32_t value = random_state_;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    random_state_ = value;
    return static_cast<double>(value) /
        static_cast<double>(0xFFFFFFFFu);
}

CharacterExpressionSample CharacterExpressionAnimator::stable_sample(
    StableExpression expression
) const {
    return expression == StableExpression::Base
        ? CharacterExpressionSample{
            .eyes = CharacterEyeFrame::Base,
            .mouth = CharacterMouthFrame::Base,
        }
        : CharacterExpressionSample{
            .eyes = CharacterEyeFrame::Inward,
            .mouth = CharacterMouthFrame::Curious,
        };
}

CharacterExpressionAnimator::StableExpression
CharacterExpressionAnimator::stable_expression_for_phase() const noexcept {
    switch (phase_) {
    case CharacterExpressionPhase::Paused:
    case CharacterExpressionPhase::HoldingBase:
    case CharacterExpressionPhase::ClosingToInward:
        return StableExpression::Base;
    case CharacterExpressionPhase::ClosedToInward:
    case CharacterExpressionPhase::OpeningToInward:
    case CharacterExpressionPhase::SettlingInward:
    case CharacterExpressionPhase::HoldingInward:
    case CharacterExpressionPhase::ClosingToBase:
        return StableExpression::Inward;
    case CharacterExpressionPhase::ClosedToBase:
    case CharacterExpressionPhase::OpeningToBase:
    case CharacterExpressionPhase::SettlingBase:
        return StableExpression::Base;
    }
    return StableExpression::Base;
}

} // namespace realmheart::animation::character
