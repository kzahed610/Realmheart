#include "animation/character/CharacterAnimator.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::animation::character {
namespace {

constexpr CharacterAnimationSample kHiddenSample{
    .visibility = 0.0,
    .offset_x = 42.0,
    .offset_y = 3.0,
    .hair_tip_offset_x = 14.0,
};

constexpr CharacterAnimationSample kEnterOvershootSample{
    .visibility = 1.0,
    .offset_x = -4.0,
    .offset_y = -0.5,
    .hair_tip_offset_x = 10.0,
};

constexpr CharacterAnimationSample kIdleSample{
    .visibility = 1.0,
    .offset_x = 0.0,
    .offset_y = 0.0,
    .hair_tip_offset_x = 0.0,
};

constexpr CharacterAnimationSample kExitHiddenSample{
    .visibility = 0.0,
    .offset_x = 52.0,
    .offset_y = 3.0,
    .hair_tip_offset_x = -20.0,
};

bool samples_differ(
    const CharacterAnimationSample& lhs,
    const CharacterAnimationSample& rhs
) {
    constexpr double epsilon = 0.0001;
    return std::abs(lhs.visibility - rhs.visibility) > epsilon ||
        std::abs(lhs.offset_x - rhs.offset_x) > epsilon ||
        std::abs(lhs.offset_y - rhs.offset_y) > epsilon ||
        std::abs(lhs.hair_tip_offset_x - rhs.hair_tip_offset_x) > epsilon;
}

} // namespace

CharacterAnimator::CharacterAnimator() {
    snap_hidden();
}

void CharacterAnimator::snap_hidden() {
    phase_ = CharacterAnimationPhase::Hidden;
    sample_ = kHiddenSample;
    transition_start_ = sample_;
    transition_target_ = sample_;
    transition_elapsed_seconds_ = 0.0;
    transition_duration_seconds_ = 0.0;
}

void CharacterAnimator::snap_idle() {
    phase_ = CharacterAnimationPhase::Idle;
    sample_ = kIdleSample;
    transition_start_ = sample_;
    transition_target_ = sample_;
    transition_elapsed_seconds_ = 0.0;
    transition_duration_seconds_ = 0.0;
}

void CharacterAnimator::start_enter() {
    if (phase_ == CharacterAnimationPhase::Idle ||
        phase_ == CharacterAnimationPhase::Settling) {
        return;
    }

    begin_transition(
        CharacterAnimationPhase::Entering,
        kEnterDurationSeconds,
        kEnterOvershootSample
    );
}

void CharacterAnimator::start_exit() {
    if (phase_ == CharacterAnimationPhase::Hidden ||
        phase_ == CharacterAnimationPhase::Exiting) {
        return;
    }

    begin_transition(
        CharacterAnimationPhase::Exiting,
        kExitDurationSeconds,
        kExitHiddenSample
    );
}

bool CharacterAnimator::advance(double delta_seconds) {
    if (!active()) return false;

    const CharacterAnimationSample before = sample_;
    const CharacterAnimationPhase phase_before = phase_;
    double remaining = std::clamp(delta_seconds, 0.0, 0.50);

    while (remaining > 0.0 && active()) {
        const double duration = std::max(transition_duration_seconds_, 0.000001);
        const double available = std::max(duration - transition_elapsed_seconds_, 0.0);
        const double consumed = std::min(remaining, available);
        transition_elapsed_seconds_ += consumed;
        remaining -= consumed;

        const double linear_progress = std::clamp(
            transition_elapsed_seconds_ / duration,
            0.0,
            1.0
        );
        const double eased_progress = phase_ == CharacterAnimationPhase::Exiting
            ? ease_in_cubic(linear_progress)
            : ease_out_cubic(linear_progress);
        sample_ = interpolate(
            transition_start_,
            transition_target_,
            eased_progress
        );

        if (linear_progress >= 1.0) finish_transition();
        if (consumed <= 0.0) break;
    }

    return phase_before != phase_ || samples_differ(before, sample_);
}

bool CharacterAnimator::active() const noexcept {
    return phase_ == CharacterAnimationPhase::Entering ||
        phase_ == CharacterAnimationPhase::Settling ||
        phase_ == CharacterAnimationPhase::Exiting;
}

double CharacterAnimator::display_opacity() const noexcept {
    if (phase_ != CharacterAnimationPhase::Exiting) return sample_.visibility;

    constexpr double hold_seconds = 0.06;
    if (transition_elapsed_seconds_ <= hold_seconds) {
        return transition_start_.visibility;
    }

    const double fade_duration = std::max(
        transition_duration_seconds_ - hold_seconds,
        0.000001
    );
    const double fade_progress = std::clamp(
        (transition_elapsed_seconds_ - hold_seconds) / fade_duration,
        0.0,
        1.0
    );
    const double eased = ease_in_cubic(fade_progress);
    return transition_start_.visibility +
        ((transition_target_.visibility - transition_start_.visibility) * eased);
}

double CharacterAnimator::idle_hair_wave(
    double elapsed_seconds,
    double phase_radians
) noexcept {
    const double time = std::max(elapsed_seconds, 0.0);
    const double ramp_progress = std::clamp(time / 0.40, 0.0, 1.0);
    const double startup_ramp = ramp_progress * ramp_progress *
        (3.0 - (2.0 * ramp_progress));

    const double activity_phase = (time * 0.14) + 0.90;
    const double activity_wave = 0.5 + (0.5 * std::sin(activity_phase));
    const double activity = 0.45 + (0.55 * activity_wave * activity_wave);

    const double primary = std::sin((time * 0.78) + phase_radians);
    const double secondary = std::sin(
        (time * 1.22) + (phase_radians * 1.55) + 0.39
    );
    const double drift = std::sin(
        (time * 0.33) + (phase_radians * 0.57) - 0.24
    );
    const double combined =
        (0.72 * primary) + (0.20 * secondary) + (0.08 * drift);

    return std::clamp(startup_ramp * activity * combined, -1.0, 1.0);
}

void CharacterAnimator::begin_transition(
    CharacterAnimationPhase phase,
    double duration_seconds,
    CharacterAnimationSample target
) {
    phase_ = phase;
    transition_start_ = sample_;
    transition_target_ = target;
    transition_elapsed_seconds_ = 0.0;
    transition_duration_seconds_ = duration_seconds;
}

void CharacterAnimator::finish_transition() {
    sample_ = transition_target_;

    switch (phase_) {
    case CharacterAnimationPhase::Entering:
        begin_transition(
            CharacterAnimationPhase::Settling,
            kSettleDurationSeconds,
            kIdleSample
        );
        break;
    case CharacterAnimationPhase::Settling:
        snap_idle();
        break;
    case CharacterAnimationPhase::Exiting:
        snap_hidden();
        break;
    case CharacterAnimationPhase::Hidden:
    case CharacterAnimationPhase::Idle:
        break;
    }
}

double CharacterAnimator::ease_out_cubic(double progress) {
    const double inverse = 1.0 - std::clamp(progress, 0.0, 1.0);
    return 1.0 - (inverse * inverse * inverse);
}

double CharacterAnimator::ease_in_cubic(double progress) {
    const double clamped = std::clamp(progress, 0.0, 1.0);
    return clamped * clamped * clamped;
}

CharacterAnimationSample CharacterAnimator::interpolate(
    const CharacterAnimationSample& from,
    const CharacterAnimationSample& to,
    double progress
) {
    const double clamped = std::clamp(progress, 0.0, 1.0);
    return {
        .visibility = from.visibility +
            ((to.visibility - from.visibility) * clamped),
        .offset_x = from.offset_x + ((to.offset_x - from.offset_x) * clamped),
        .offset_y = from.offset_y + ((to.offset_y - from.offset_y) * clamped),
        .hair_tip_offset_x = from.hair_tip_offset_x +
            ((to.hair_tip_offset_x - from.hair_tip_offset_x) * clamped),
    };
}

} // namespace realmheart::animation::character
