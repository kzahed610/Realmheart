#include "ui/lockscreen/LockStateMachine.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace realmheart::ui::lockscreen {
namespace {

TEST(LockStateMachine, StartsHidden) {
    LockStateMachine machine;
    EXPECT_EQ(machine.phase(), LockPhase::Hidden);
    EXPECT_EQ(machine.progress(), 0.0);
    EXPECT_FALSE(machine.needs_frame());
}

TEST(LockStateMachine, PresentEntersOpening) {
    LockStateMachine machine;
    machine.present();
    EXPECT_EQ(machine.phase(), LockPhase::Opening);
    EXPECT_EQ(machine.progress(), 0.0);
    EXPECT_TRUE(machine.needs_frame());
}

TEST(LockStateMachine, OpeningAdvancesToSplittingThenTyping) {
    LockStateMachine machine;
    machine.present();

    // 0.70s opening -> Splitting.
    machine.advance(0.35);
    EXPECT_EQ(machine.phase(), LockPhase::Opening);
    machine.advance(0.35);
    EXPECT_EQ(machine.phase(), LockPhase::Splitting);
    EXPECT_EQ(machine.progress(), 0.0);

    // 0.90s splitting -> Typing (idle, no frames).
    machine.advance(0.45);
    EXPECT_EQ(machine.phase(), LockPhase::Splitting);
    machine.advance(0.45);
    EXPECT_EQ(machine.phase(), LockPhase::Typing);
    EXPECT_FALSE(machine.needs_frame());
}

TEST(LockStateMachine, DismissClosesFromTyping) {
    LockStateMachine machine;
    machine.present();
    machine.advance(1.0); // Opening -> Splitting
    machine.advance(1.0); // Splitting -> Typing
    EXPECT_EQ(machine.phase(), LockPhase::Typing);

    machine.dismiss();
    EXPECT_EQ(machine.phase(), LockPhase::Closing);
    EXPECT_TRUE(machine.needs_frame());

    machine.advance(0.30);
    EXPECT_EQ(machine.phase(), LockPhase::Hidden);
    EXPECT_FALSE(machine.needs_frame());
}

TEST(LockStateMachine, HideImmediatelyResets) {
    LockStateMachine machine;
    machine.present();
    machine.advance(0.1);
    machine.hide_immediately();
    EXPECT_EQ(machine.phase(), LockPhase::Hidden);
    EXPECT_EQ(machine.progress(), 0.0);
    EXPECT_FALSE(machine.needs_frame());
}

TEST(LockStateMachine, IgnoresNonFiniteDelta) {
    LockStateMachine machine;
    machine.present();
    machine.advance(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(machine.phase(), LockPhase::Opening);
    EXPECT_EQ(machine.progress(), 0.0);
}

TEST(LockStateMachine, ClampsLargeDelta) {
    LockStateMachine machine;
    machine.present();
    // A 10s stall must not jump past Splitting in one step.
    machine.advance(10.0);
    EXPECT_TRUE(machine.phase() == LockPhase::Opening ||
                machine.phase() == LockPhase::Splitting);
}

} // namespace
} // namespace realmheart::ui::lockscreen
