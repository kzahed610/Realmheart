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
    reset_inward();
}

void CharacterExpressionAnimator::reset_inward() {
    phase_ = CharacterExpressionPhase::Paused;
    stable_gaze_ = StableGaze::Inward;
    sample_ = stable_sample(stable_gaze_);
    phase_elapsed_seconds_ = 0.0;
    phase_duration_seconds_ = 0.0;
}

bool CharacterExpressionAnimator::resume() {
    if (active()) return false;

    const CharacterExpressionSample before = sample_;
    begin_hold(stable_gaze_);
    return samples_differ(before, sample_);
}

bool CharacterExpressionAnimator::pause_stable() {
    const CharacterExpressionSample before = sample_;
    stable_gaze_ = stable_gaze_for_phase();
    sample_ = stable_sample(stable_gaze_);
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

void CharacterExpressionAnimator::begin_hold(StableGaze gaze) {
    stable_gaze_ = gaze;
    begin_phase(
        gaze == StableGaze::Inward
            ? CharacterExpressionPhase::HoldingInward
            : CharacterExpressionPhase::HoldingUser,
        randomized_hold_seconds(gaze)
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
        sample_ = stable_sample(stable_gaze_);
        break;
    case CharacterExpressionPhase::HoldingInward:
        stable_gaze_ = StableGaze::Inward;
        sample_ = stable_sample(stable_gaze_);
        break;
    case CharacterExpressionPhase::ClosingToUser:
        sample_ = {
            .eyes = CharacterEyeFrame::Half,
            .mouth = CharacterMouthFrame::Curious,
        };
        break;
    case CharacterExpressionPhase::ClosedToUser:
        // The gaze swaps while the eyes are shut, but the mouth deliberately
        // remains curious. Tessia notices the user before reacting with a smile.
        stable_gaze_ = StableGaze::User;
        sample_ = {
            .eyes = CharacterEyeFrame::Closed,
            .mouth = CharacterMouthFrame::Curious,
        };
        break;
    case CharacterExpressionPhase::OpeningToUser:
        sample_ = {
            .eyes = CharacterEyeFrame::Half,
            .mouth = CharacterMouthFrame::Curious,
        };
        break;
    case CharacterExpressionPhase::SettlingUser:
        sample_ = {
            .eyes = CharacterEyeFrame::User,
            .mouth = CharacterMouthFrame::Curious,
        };
        break;
    case CharacterExpressionPhase::HoldingUser:
        stable_gaze_ = StableGaze::User;
        sample_ = stable_sample(stable_gaze_);
        break;
    case CharacterExpressionPhase::ClosingToInward:
        sample_ = {
            .eyes = CharacterEyeFrame::Half,
            .mouth = CharacterMouthFrame::Smile,
        };
        break;
    case CharacterExpressionPhase::ClosedToInward:
        stable_gaze_ = StableGaze::Inward;
        sample_ = {
            .eyes = CharacterEyeFrame::Closed,
            .mouth = CharacterMouthFrame::Smile,
        };
        break;
    case CharacterExpressionPhase::OpeningToInward:
        sample_ = {
            .eyes = CharacterEyeFrame::Half,
            .mouth = CharacterMouthFrame::Smile,
        };
        break;
    case CharacterExpressionPhase::SettlingInward:
        sample_ = {
            .eyes = CharacterEyeFrame::Inward,
            .mouth = CharacterMouthFrame::Smile,
        };
        break;
    }
}

void CharacterExpressionAnimator::finish_phase() {
    switch (phase_) {
    case CharacterExpressionPhase::Paused:
        break;
    case CharacterExpressionPhase::HoldingInward:
        begin_phase(
            CharacterExpressionPhase::ClosingToUser,
            kBlinkCloseSeconds
        );
        break;
    case CharacterExpressionPhase::ClosingToUser:
        begin_phase(
            CharacterExpressionPhase::ClosedToUser,
            kBlinkClosedSeconds
        );
        break;
    case CharacterExpressionPhase::ClosedToUser:
        begin_phase(
            CharacterExpressionPhase::OpeningToUser,
            kBlinkOpenSeconds
        );
        break;
    case CharacterExpressionPhase::OpeningToUser:
        begin_phase(
            CharacterExpressionPhase::SettlingUser,
            kUserReactionDelaySeconds
        );
        break;
    case CharacterExpressionPhase::SettlingUser:
        begin_hold(StableGaze::User);
        break;
    case CharacterExpressionPhase::HoldingUser:
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
        begin_hold(StableGaze::Inward);
        break;
    }
}

double CharacterExpressionAnimator::randomized_hold_seconds(StableGaze gaze) {
    const double minimum = gaze == StableGaze::Inward ? 3.2 : 2.6;
    const double maximum = gaze == StableGaze::Inward ? 5.0 : 4.0;
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
    // xorshift32: tiny, deterministic, and more than sufficient for timing
    // variation. No platform RNG or global state is involved.
    std::uint32_t value = random_state_;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    random_state_ = value;
    return static_cast<double>(value) /
        static_cast<double>(0xFFFFFFFFu);
}

CharacterExpressionSample CharacterExpressionAnimator::stable_sample(
    StableGaze gaze
) const {
    return gaze == StableGaze::Inward
        ? CharacterExpressionSample{
            .eyes = CharacterEyeFrame::Inward,
            .mouth = CharacterMouthFrame::Curious,
        }
        : CharacterExpressionSample{
            .eyes = CharacterEyeFrame::User,
            .mouth = CharacterMouthFrame::Smile,
        };
}

CharacterExpressionAnimator::StableGaze
CharacterExpressionAnimator::stable_gaze_for_phase() const noexcept {
    switch (phase_) {
    case CharacterExpressionPhase::Paused:
    case CharacterExpressionPhase::HoldingInward:
    case CharacterExpressionPhase::ClosingToUser:
        return StableGaze::Inward;
    case CharacterExpressionPhase::ClosedToUser:
    case CharacterExpressionPhase::OpeningToUser:
    case CharacterExpressionPhase::SettlingUser:
    case CharacterExpressionPhase::HoldingUser:
    case CharacterExpressionPhase::ClosingToInward:
        return StableGaze::User;
    case CharacterExpressionPhase::ClosedToInward:
    case CharacterExpressionPhase::OpeningToInward:
    case CharacterExpressionPhase::SettlingInward:
        return StableGaze::Inward;
    }
    return StableGaze::Inward;
}

} // namespace realmheart::animation::character
