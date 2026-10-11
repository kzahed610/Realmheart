#include "wallpaper-native/video/FrameScheduler.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;
using realmheart::wallpaper_native::FrameScheduler;

namespace {

using FrameView = FrameScheduler::FrameView;

std::vector<FrameView> frames(std::initializer_list<std::int64_t> times_us) {
    std::vector<FrameView> result;
    result.reserve(times_us.size());
    for (const auto time : times_us) {
        FrameView view;
        view.presentation_us = time;
        view.has_timestamp = true;
        result.push_back(view);
    }
    return result;
}

std::vector<FrameView> untimed(std::size_t count) {
    std::vector<FrameView> result(count);
    return result;
}

bool is_present(const FrameScheduler::Decision& decision) {
    return decision.action == FrameScheduler::Decision::Action::Present;
}

bool is_wait(const FrameScheduler::Decision& decision) {
    return decision.action == FrameScheduler::Decision::Action::Wait;
}

bool is_idle(const FrameScheduler::Decision& decision) {
    return decision.action == FrameScheduler::Decision::Action::Idle;
}

} // namespace

// No source, no frames: the renderer must have no timed work at all.
TEST(FrameSchedulerTest, NoSourceHasNoDeadline) {
    FrameScheduler scheduler;
    const auto now = std::chrono::steady_clock::now();
    EXPECT_FALSE(scheduler.next_deadline(now, frames({0})).has_value());
    EXPECT_TRUE(is_idle(scheduler.decide(now, frames({0}))));
}

TEST(FrameSchedulerTest, EmptyQueueIsIdle) {
    FrameScheduler scheduler;
    const auto now = std::chrono::steady_clock::now();
    scheduler.start(now);
    EXPECT_FALSE(scheduler.next_deadline(now, {}).has_value());
    EXPECT_TRUE(is_idle(scheduler.decide(now, {})));
}

TEST(FrameSchedulerTest, FirstFrameIsPresentedOnlyWhenDue) {
    FrameScheduler scheduler;
    const auto now = std::chrono::steady_clock::now();
    scheduler.start(now);

    const auto queued = frames({0});
    EXPECT_TRUE(is_present(scheduler.decide(now, queued)));

    // A frame one interval in the future must wait, not play early.
    const auto start = std::chrono::steady_clock::now();
    FrameScheduler future;
    future.start(start);
    const auto later = frames({41'667});
    const auto decision = future.decide(start + 10ms, later);
    EXPECT_TRUE(is_wait(decision));
    EXPECT_EQ(decision.deadline, start + 41ms + 667us);
    ASSERT_TRUE(future.next_deadline(start + 10ms, later).has_value());
    EXPECT_EQ(*future.next_deadline(start + 10ms, later), start + 41ms + 667us);
}

// A 24/30/60 FPS timeline must produce deadlines at those rates, not at the
// monitor refresh rate.
TEST(FrameSchedulerTest, PresentationTracksTheMediaRate) {
    struct Case {
        std::int64_t frame_us;
        std::chrono::steady_clock::duration interval;
    };
    const std::vector<Case> cases = {
        {41'667, std::chrono::microseconds(41'667)},   // 24 FPS
        {33'333, std::chrono::microseconds(33'333)},   // 30 FPS
        {16'667, std::chrono::microseconds(16'667)},   // 60 FPS
    };

    for (const auto& entry : cases) {
        FrameScheduler scheduler;
        const auto start = std::chrono::steady_clock::now();
        scheduler.start(start);

        // The first frame is due at playback start.
        EXPECT_TRUE(is_present(scheduler.decide(start, frames({0, entry.frame_us}))));
        // The next deadline is exactly one media interval later: a 24 FPS video
        // wakes the loop every 41.667 ms, not every monitor refresh.
        const auto second_deadline =
            scheduler.next_deadline(start, frames({entry.frame_us}));
        ASSERT_TRUE(second_deadline.has_value());
        EXPECT_EQ(*second_deadline - start, entry.interval);

        // Not due halfway to its own deadline, whatever the rate.
        EXPECT_TRUE(is_wait(scheduler.decide(
            start + entry.interval / 2, frames({entry.frame_us}))));
        // Due once its own interval elapsed.
        const auto due = scheduler.decide(start + entry.interval, frames({entry.frame_us}));
        EXPECT_TRUE(is_present(due));
        EXPECT_EQ(due.frame_index, 0U);
    }
}

// The first timestamp is normalized to playback zero by the decoder, so a
// non-zero first PTS must still schedule correctly here.
TEST(FrameSchedulerTest, NonZeroFirstTimestampStillSchedules) {
    FrameScheduler scheduler;
    const auto start = std::chrono::steady_clock::now();
    scheduler.start(start);
    // A decoder that did not normalize: first frame is 500 ms into the file.
    const auto queued = frames({500'000, 541'667});
    const auto decision = scheduler.decide(start + 500ms, queued);
    EXPECT_TRUE(is_present(decision));
    EXPECT_EQ(decision.frame_index, 0U);
}

TEST(FrameSchedulerTest, LateByOneFrameDropsTheMissedFrame) {
    FrameScheduler scheduler;
    const auto start = std::chrono::steady_clock::now();
    scheduler.start(start);

    const auto queued = frames({0, 41'667, 83'333});
    const auto decision = scheduler.decide(start + 50ms, queued);
    EXPECT_TRUE(is_present(decision));
    // Show the newest frame that is due; the skipped one is dropped, not
    // presented in sequence.
    EXPECT_EQ(decision.frame_index, 1U);
    EXPECT_EQ(decision.drop_before, 1U);
    EXPECT_EQ(scheduler.dropped_frames(), 1U);
}

TEST(FrameSchedulerTest, LateByManyFramesDoesNotDrainBacklog) {
    FrameScheduler scheduler;
    const auto start = std::chrono::steady_clock::now();
    scheduler.start(start);

    std::vector<FrameView> queued;
    for (int index = 0; index < 10; ++index) {
        FrameView view;
        view.presentation_us = static_cast<std::int64_t>(index) * 41'667;
        view.has_timestamp = true;
        queued.push_back(view);
    }

    const auto decision = scheduler.decide(start + 200ms, queued);
    EXPECT_TRUE(is_present(decision));
    EXPECT_EQ(decision.frame_index, 4U);
    EXPECT_EQ(decision.drop_before, 4U);
    EXPECT_EQ(scheduler.dropped_frames(), 4U);
}

// Media without usable timestamps falls back to the configured frame rate,
// and the fallback stays in step across calls.
TEST(FrameSchedulerTest, MissingTimestampsFallBackToFrameRate) {
    FrameScheduler scheduler;
    scheduler.set_frames_per_second(30.0);
    const auto start = std::chrono::steady_clock::now();
    scheduler.start(start);

    // First untimed frame is due immediately.
    EXPECT_TRUE(is_present(scheduler.decide(start, untimed(3))));
    scheduler.note_consumed(1);
    // The next one is due one 30 FPS interval later (33333 us), not before.
    const auto second = scheduler.decide(start + 33ms + 400us, untimed(2));
    EXPECT_TRUE(is_present(second));
    EXPECT_EQ(second.frame_index, 0U);
    scheduler.note_consumed(1);
    // Two frame intervals have now elapsed, so the third is not due yet.
    EXPECT_TRUE(is_wait(scheduler.decide(start + 34ms, untimed(1))));
}

TEST(FrameSchedulerTest, MissingTimestampsWithoutRateHaveNoTimedWork) {
    FrameScheduler scheduler;
    const auto start = std::chrono::steady_clock::now();
    scheduler.start(start);
    // No timestamps and no known rate: never spin, never wake on a bogus
    // far-future deadline.
    EXPECT_FALSE(scheduler.next_deadline(start, untimed(3)).has_value());
    const auto decision = scheduler.decide(start + 1h, untimed(3));
    EXPECT_TRUE(is_wait(decision) || is_idle(decision));
}

TEST(FrameSchedulerTest, LoopRestartReAnchorsTheTimeline) {
    FrameScheduler scheduler;
    const auto start = std::chrono::steady_clock::now();
    scheduler.start(start);
    scheduler.set_frames_per_second(10.0);

    scheduler.note_consumed(5);
    ASSERT_EQ(scheduler.loop_restarts(), 0U);

    const auto loop_start = start + 2s;
    scheduler.restart(loop_start);
    EXPECT_EQ(scheduler.loop_restarts(), 1U);

    // After the loop the first frame is due immediately again, even though
    // five frames were shown on the previous pass.
    EXPECT_TRUE(is_present(scheduler.decide(loop_start, untimed(1))));
    scheduler.note_consumed(1);
    // The next frame of the new pass is one interval out, measured from the
    // loop start rather than the old timeline.
    EXPECT_TRUE(is_wait(scheduler.decide(loop_start + 99ms, untimed(1))));
    ASSERT_TRUE(scheduler.next_deadline(loop_start + 99ms, untimed(1)).has_value());
    EXPECT_EQ(*scheduler.next_deadline(loop_start + 99ms, untimed(1)) - loop_start,
              100ms);
}

TEST(FrameSchedulerTest, ClockSteppingBackwardsReAnchors) {
    FrameScheduler scheduler;
    const auto start = std::chrono::steady_clock::now();
    scheduler.start(start);
    scheduler.set_frames_per_second(10.0);

    const auto queued = untimed(3);
    scheduler.note_consumed(3);

    // A clock step backwards (or a start in the future) would otherwise wedge
    // every frame as permanently late.
    const auto backwards = start - 5s;
    const auto decision = scheduler.decide(backwards, queued);
    EXPECT_FALSE(is_wait(decision));
    EXPECT_EQ(scheduler.clock_resets(), 1U);
    // The re-anchored schedule is usable again.
    EXPECT_TRUE(is_present(scheduler.decide(backwards, queued)));
}

TEST(FrameSchedulerTest, StopClearsAllTimedWork) {
    FrameScheduler scheduler;
    const auto start = std::chrono::steady_clock::now();
    scheduler.start(start);
    scheduler.set_frames_per_second(30.0);
    scheduler.note_consumed(2);

    scheduler.stop();
    EXPECT_FALSE(scheduler.running());
    EXPECT_FALSE(scheduler.next_deadline(start, untimed(1)).has_value());
    EXPECT_TRUE(is_idle(scheduler.decide(start + 1h, untimed(1))));
}

// The deadline the renderer polls on must never be a far-future sentinel: that
// would look like timed work and wake the loop for nothing.
TEST(FrameSchedulerTest, SaturatingDeadlineIsNeverReported) {
    FrameScheduler scheduler;
    const auto start = std::chrono::steady_clock::now();
    scheduler.start(start);

    FrameView untimed_frame;
    untimed_frame.has_timestamp = false;
    untimed_frame.presentation_us = 1'000'000;
    const std::vector<FrameView> queued{untimed_frame};

    EXPECT_FALSE(scheduler.next_deadline(start, queued).has_value());
    EXPECT_FALSE(scheduler.next_deadline(start + 1h, queued).has_value());
}

TEST(FrameSchedulerTest, PresentedFramesAreCountedOnce) {
    FrameScheduler scheduler;
    const auto start = std::chrono::steady_clock::now();
    scheduler.start(start);
    scheduler.set_frames_per_second(10.0);

    EXPECT_TRUE(is_present(scheduler.decide(start, untimed(1))));
    scheduler.note_consumed(1);
    EXPECT_TRUE(is_present(scheduler.decide(start + 100ms, untimed(1))));
    scheduler.note_consumed(1);
    EXPECT_TRUE(is_present(scheduler.decide(start + 200ms, untimed(1))));
    scheduler.note_consumed(1);
    // After three frames at 10 FPS, the fourth is 300 ms out.
    const auto next = scheduler.next_deadline(start, untimed(1));
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(*next - start, 300ms);
}
