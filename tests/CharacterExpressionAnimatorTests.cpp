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

void test_starts_paused_on_authored_base_face() {
    CharacterExpressionAnimator animator;
    require(!animator.active(), "expression animator must start paused");
    require(animator.sample().eyes == CharacterEyeFrame::Base,
            "default gaze must reveal the eyes authored in base.png");
    require(animator.sample().mouth == CharacterMouthFrame::Base,
            "default smile must reveal the mouth authored in base.png");
    require(!animator.advance(1.0),
            "paused expressions must perform zero timing work");
}

void test_first_change_blinks_from_base_to_inward() {
    CharacterExpressionAnimator animator;
    animator.resume();

    double elapsed = 0.0;
    while (animator.sample().eyes == CharacterEyeFrame::Base && elapsed < 8.0) {
        animator.advance(0.01);
        elapsed += 0.01;
    }
    require(elapsed >= 2.4 && elapsed <= 6.1,
            "base-face hold must be varied but remain bounded");
    require(animator.sample().eyes == CharacterEyeFrame::Half,
            "base-to-inward transition must begin with a half-lid overlay");
    require(animator.sample().mouth == CharacterMouthFrame::Base,
            "base smile must remain visible while the blink starts closing");

    while (animator.sample().eyes != CharacterEyeFrame::Closed) {
        animator.advance(0.01);
    }
    require(animator.sample().mouth == CharacterMouthFrame::Curious,
            "curious mouth overlay should appear while the eyes are closed");

    while (animator.sample().eyes != CharacterEyeFrame::Inward) {
        animator.advance(0.01);
    }
    require(animator.sample().mouth == CharacterMouthFrame::Curious,
            "inward gaze must settle with the curious mouth overlay");
}

void test_pause_resolves_partial_blink_to_base_face() {
    CharacterExpressionAnimator animator;
    animator.resume();

    while (animator.sample().eyes == CharacterEyeFrame::Base) {
        animator.advance(0.02);
    }
    require(animator.sample().eyes == CharacterEyeFrame::Half,
            "test must interrupt the closing blink");

    require(animator.pause_stable(),
            "pausing a partial blink must change the displayed frame");
    require(!animator.active(), "paused expression must stop all timing");
    require(animator.sample().eyes == CharacterEyeFrame::Base,
            "early closing blink must resolve back to the authored base gaze");
    require(animator.sample().mouth == CharacterMouthFrame::Base,
            "early closing blink must restore the authored base smile");
}

void test_round_trip_returns_to_base_face() {
    CharacterExpressionAnimator animator;
    animator.resume();

    bool saw_inward = false;
    for (int frame = 0; frame < 2000; ++frame) {
        animator.advance(0.01);
        if (animator.sample().eyes == CharacterEyeFrame::Inward) saw_inward = true;
        if (saw_inward &&
            animator.sample().eyes == CharacterEyeFrame::Base &&
            animator.sample().mouth == CharacterMouthFrame::Base) {
            return;
        }
    }
    require(false, "expression loop must return to the authored base face");
}

} // namespace

int main() {
    test_starts_paused_on_authored_base_face();
    test_first_change_blinks_from_base_to_inward();
    test_pause_resolves_partial_blink_to_base_face();
    test_round_trip_returns_to_base_face();
    std::cout << "Character expression animator tests passed\n";
    return 0;
}
