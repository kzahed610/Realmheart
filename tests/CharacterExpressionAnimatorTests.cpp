#include "animation/character/CharacterExpressionAnimator.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using realmheart::animation::character::CharacterExpressionAnimator;
using realmheart::animation::character::CharacterEyeFrame;
using realmheart::animation::character::CharacterMouthFrame;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_starts_paused_with_inward_curious_face() {
    CharacterExpressionAnimator animator;
    require(!animator.active(), "expression animator must start paused");
    require(animator.sample().eyes == CharacterEyeFrame::Inward,
            "initial gaze must face inward toward the widgets");
    require(animator.sample().mouth == CharacterMouthFrame::Curious,
            "initial mouth must use the curious frame");
    require(!animator.advance(1.0),
            "paused expressions must perform zero timing work");
}

void test_first_gaze_change_uses_complete_blink_sequence() {
    CharacterExpressionAnimator animator;
    animator.resume();

    double elapsed = 0.0;
    while (animator.sample().eyes == CharacterEyeFrame::Inward && elapsed < 8.0) {
        animator.advance(0.01);
        elapsed += 0.01;
    }
    require(elapsed >= 3.0 && elapsed <= 7.1,
            "inward hold must be varied but remain bounded");
    require(animator.sample().eyes == CharacterEyeFrame::Half,
            "gaze transition must begin with a half-lid frame");
    require(animator.sample().mouth == CharacterMouthFrame::Curious,
            "mouth must remain curious while the blink closes");

    double transition_elapsed = 0.0;
    while (animator.sample().eyes != CharacterEyeFrame::Closed) {
        animator.advance(0.01);
        transition_elapsed += 0.01;
    }
    require(animator.sample().mouth == CharacterMouthFrame::Curious,
            "mouth must remain curious while the gaze swaps behind closed eyes");

    while (animator.sample().eyes != CharacterEyeFrame::User) {
        animator.advance(0.01);
        transition_elapsed += 0.01;
    }
    require(animator.sample().mouth == CharacterMouthFrame::Curious,
            "Tessia should look at the user before reacting with a smile");

    while (animator.sample().mouth != CharacterMouthFrame::Smile) {
        animator.advance(0.01);
        transition_elapsed += 0.01;
    }
    require(transition_elapsed >= 0.50,
            "inward-to-user transition must feel deliberate rather than instant");
    require(animator.sample().eyes == CharacterEyeFrame::User,
            "smile must arrive only after the user-facing gaze has settled");
}

void test_pause_resolves_partial_blink_to_stable_face() {
    CharacterExpressionAnimator animator;
    animator.resume();

    while (animator.sample().eyes == CharacterEyeFrame::Inward) {
        animator.advance(0.02);
    }
    require(animator.sample().eyes == CharacterEyeFrame::Half,
            "test must interrupt the closing blink");

    require(animator.pause_stable(),
            "pausing a partial blink must change the displayed frame");
    require(!animator.active(), "paused expression must stop all timing");
    require(animator.sample().eyes == CharacterEyeFrame::Inward,
            "early closing blink must resolve back to inward gaze");
    require(animator.sample().mouth == CharacterMouthFrame::Curious,
            "resolved inward gaze must restore the curious mouth");
}

void test_round_trip_returns_to_inward_face() {
    CharacterExpressionAnimator animator;
    animator.resume();

    bool saw_user = false;
    for (int frame = 0; frame < 2000; ++frame) {
        animator.advance(0.01);
        if (animator.sample().eyes == CharacterEyeFrame::User) saw_user = true;
        if (saw_user &&
            animator.sample().eyes == CharacterEyeFrame::Inward &&
            animator.sample().mouth == CharacterMouthFrame::Curious) {
            return;
        }
    }
    require(false, "expression loop must return inward within a bounded interval");
}

} // namespace

int main() {
    test_starts_paused_with_inward_curious_face();
    test_first_gaze_change_uses_complete_blink_sequence();
    test_pause_resolves_partial_blink_to_stable_face();
    test_round_trip_returns_to_inward_face();
    std::cout << "Character expression animator tests passed\n";
    return 0;
}
