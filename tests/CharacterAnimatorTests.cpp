#include "animation/character/CharacterAnimator.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using realmheart::animation::character::CharacterAnimationPhase;
using realmheart::animation::character::CharacterAnimator;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_hidden_enter_settle_idle_lifecycle() {
    CharacterAnimator animator;
    require(animator.hidden(), "animator must start hidden");
    require(animator.sample().visibility == 0.0, "hidden character must be transparent");

    animator.start_enter();
    require(animator.phase() == CharacterAnimationPhase::Entering,
            "enter request must begin the entering phase");
    require(animator.active(), "entering animation must be active");

    animator.advance(CharacterAnimator::kEnterDurationSeconds);
    require(animator.phase() == CharacterAnimationPhase::Settling,
            "entry must transition into a distinct settling phase");
    require(animator.sample().visibility == 1.0,
            "character must be fully visible before settling");
    require(animator.sample().offset_x < 0.0,
            "entry must overshoot slightly before settling");
    require(animator.sample().hair_tip_offset_x > 0.0,
            "hair tips must still trail behind the body at entry overshoot");

    animator.advance(CharacterAnimator::kSettleDurationSeconds);
    require(animator.idle(), "settling must finish in idle");
    require(!animator.active(), "idle character must not require frame ticks");
    require(std::abs(animator.sample().offset_x) < 0.0001,
            "idle character must return to its authored anchor");
    require(std::abs(animator.sample().hair_tip_offset_x) < 0.0001,
            "idle hair must finish exactly on its authored texture geometry");
}

void test_exit_reaches_zero_work_hidden_state() {
    CharacterAnimator animator;
    animator.snap_idle();
    animator.start_exit();
    require(animator.phase() == CharacterAnimationPhase::Exiting,
            "exit request must begin the exiting phase");

    animator.advance(CharacterAnimator::kExitDurationSeconds);
    require(animator.hidden(), "exit must finish hidden");
    require(!animator.active(), "hidden character must not keep an animation tick alive");
    require(animator.sample().visibility == 0.0,
            "hidden character must finish fully transparent");
    require(animator.sample().offset_x > 0.0,
            "exit must retreat behind the host surface");
    require(animator.sample().hair_tip_offset_x > 0.0,
            "fully hidden state must reset hair for the next entry");
}

void test_exit_holds_opacity_before_fade() {
    CharacterAnimator animator;
    animator.snap_idle();
    animator.start_exit();

    animator.advance(0.05);
    require(animator.sample().offset_x > 0.0,
            "exit motion must begin during the early opacity-hold window");
    require(std::abs(animator.display_opacity() - 1.0) < 0.0001,
            "exit should remain fully opaque briefly before fading");

    animator.advance(0.08);
    require(animator.display_opacity() < 1.0,
            "exit opacity must begin fading after the hold window");
}

void test_idle_hair_wave_is_bounded_and_starts_without_a_pop() {
    require(std::abs(CharacterAnimator::idle_hair_wave(0.0, 0.82)) < 0.0001,
            "idle wave must start at zero displacement");

    bool observed_motion = false;
    for (int index = 1; index <= 600; ++index) {
        const double sample = CharacterAnimator::idle_hair_wave(
            static_cast<double>(index) * 0.05,
            0.82
        );
        require(sample >= -1.0001 && sample <= 1.0001,
                "idle wave must remain normalized");
        if (std::abs(sample) > 0.05) observed_motion = true;
    }
    require(observed_motion, "idle wave must eventually produce visible motion");
}

void test_reverse_from_exit_reuses_current_sample() {
    CharacterAnimator animator;
    animator.snap_idle();
    animator.start_exit();
    animator.advance(CharacterAnimator::kExitDurationSeconds * 0.5);
    const double interrupted_offset = animator.sample().offset_x;
    const double interrupted_hair_offset = animator.sample().hair_tip_offset_x;
    require(interrupted_hair_offset < 0.0,
            "hair tips must trail opposite the exiting body before reversal");

    animator.start_enter();
    require(animator.phase() == CharacterAnimationPhase::Entering,
            "reopening during exit must reverse into entering");
    require(std::abs(animator.sample().offset_x - interrupted_offset) < 0.0001,
            "reversal must not jump to a canned starting position");
    require(std::abs(
                animator.sample().hair_tip_offset_x - interrupted_hair_offset
            ) < 0.0001,
            "hair inertia must also reverse without a positional jump");
}

} // namespace

int main() {
    test_hidden_enter_settle_idle_lifecycle();
    test_exit_reaches_zero_work_hidden_state();
    test_exit_holds_opacity_before_fade();
    test_idle_hair_wave_is_bounded_and_starts_without_a_pop();
    test_reverse_from_exit_reuses_current_sample();
    std::cout << "Character animator tests passed\n";
    return 0;
}
