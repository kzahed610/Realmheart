#include "wallpaper-native/video/VideoFrameQueue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stop_token>
#include <thread>

using realmheart::wallpaper_native::VideoFrame;
using realmheart::wallpaper_native::VideoFrameFormat;
using realmheart::wallpaper_native::VideoFrameQueue;

using namespace std::chrono_literals;

namespace {

VideoFrame make_frame(int value) {
    VideoFrame frame;
    frame.format = VideoFrameFormat::Rgba8;
    frame.width = 2;
    frame.height = 2;
    frame.pixels.assign(2 * 2 * 4U, static_cast<unsigned char>(value));
    frame.presentation_us = static_cast<std::int64_t>(value);
    frame.has_timestamp = true;
    return frame;
}

int pixel_value(const VideoFrame& frame) {
    return frame.pixels.empty() ? -1 : frame.pixels.front();
}

// Waiting for a condition is the only honest way to observe a worker thread:
// a single sleep is a race, and a deadlock would otherwise look like a race.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

} // namespace

TEST(VideoFrameQueueTest, PushAndPopPreserveOrder) {
    VideoFrameQueue queue(3);
    ASSERT_TRUE(queue.push(make_frame(1), 7, {}));
    ASSERT_TRUE(queue.push(make_frame(2), 7, {}));
    ASSERT_TRUE(queue.push(make_frame(3), 7, {}));
    EXPECT_EQ(queue.size(), 3U);
    EXPECT_EQ(queue.high_water(), 3U);

    VideoFrame frame;
    std::uint64_t generation = 0;
    ASSERT_TRUE(queue.pop(frame, generation));
    EXPECT_EQ(pixel_value(frame), 1);
    EXPECT_EQ(generation, 7U);
    ASSERT_TRUE(queue.pop(frame, generation));
    EXPECT_EQ(pixel_value(frame), 2);
    ASSERT_TRUE(queue.pop(frame, generation));
    EXPECT_EQ(pixel_value(frame), 3);
    EXPECT_FALSE(queue.pop(frame, generation));
}

// The capacity is a backstop, not a buffer to grow (renderer docs 7.2).
TEST(VideoFrameQueueTest, ProducerWaitsInsteadOfGrowing) {
    VideoFrameQueue queue(3);
    for (int index = 0; index < 3; ++index) {
        ASSERT_TRUE(queue.push(make_frame(index), 1, {}));
    }

    std::atomic<bool> finished{false};
    std::atomic<bool> accepted{true};
    std::jthread producer([&queue, &finished, &accepted](std::stop_token token) {
        accepted.store(queue.push(make_frame(99), 1, token));
        finished.store(true);
    });
    // The queue is full: the producer must still be blocked, and the size must
    // not have moved.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_FALSE(finished.load());
    EXPECT_EQ(queue.size(), 3U);

    VideoFrame frame;
    std::uint64_t generation = 0;
    ASSERT_TRUE(queue.pop(frame, generation));
    // The freed slot lets the waiting producer in; waiting for it beats sleeping
    // and hoping, and turns a deadlock into a readable failure.
    ASSERT_TRUE(wait_for([&finished] { return finished.load(); }, 2s));
    EXPECT_EQ(queue.size(), 3U);
}

TEST(VideoFrameQueueTest, CancelledProducerIsReleased) {
    VideoFrameQueue queue(1);
    ASSERT_TRUE(queue.push(make_frame(1), 1, {}));

    std::atomic<bool> push_returned{false};
    std::atomic<bool> push_result{true};
    std::jthread producer([&queue, &push_returned, &push_result](std::stop_token token) {
        push_result.store(queue.push(make_frame(2), 1, token));
        push_returned.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    producer.request_stop();
    ASSERT_TRUE(wait_for([&push_returned] { return push_returned.load(); }, 2s));
    // A cancelled push reports failure; the producer discards its frame.
    EXPECT_FALSE(push_result.load());
    EXPECT_EQ(queue.size(), 1U);
}

TEST(VideoFrameQueueTest, CloseReleasesEverythingAndWakesWaiters) {
    VideoFrameQueue queue(2);
    ASSERT_TRUE(queue.push(make_frame(1), 1, {}));
    ASSERT_TRUE(queue.push(make_frame(2), 1, {}));

    std::atomic<bool> released{false};
    std::jthread producer([&queue, &released](std::stop_token token) {
        released.store(!queue.push(make_frame(3), 1, token));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    queue.close();
    ASSERT_TRUE(wait_for([&released] { return released.load(); }, 2s));
    EXPECT_EQ(queue.size(), 0U);

    VideoFrame frame;
    std::uint64_t generation = 0;
    EXPECT_FALSE(queue.pop(frame, generation));
    // Pushing after close must never succeed.
    EXPECT_FALSE(queue.push(make_frame(4), 1, {}));
}

// A replaced source can never have a late frame rendered (13.3 / 18).
TEST(VideoFrameQueueTest, DiscardStaleDropsOlderGenerationsOnly) {
    VideoFrameQueue queue(4);
    ASSERT_TRUE(queue.push(make_frame(1), 1, {}));
    ASSERT_TRUE(queue.push(make_frame(2), 1, {}));
    ASSERT_TRUE(queue.push(make_frame(3), 2, {}));

    queue.discard_stale(2);
    EXPECT_EQ(queue.size(), 1U);

    VideoFrame frame;
    std::uint64_t generation = 0;
    ASSERT_TRUE(queue.pop(frame, generation));
    EXPECT_EQ(generation, 2U);
    EXPECT_EQ(pixel_value(frame), 3);
}

TEST(VideoFrameQueueTest, GenerationTravelsWithTheFrame) {
    VideoFrameQueue queue(2);
    ASSERT_TRUE(queue.push(make_frame(1), 11, {}));

    VideoFrame frame;
    std::uint64_t generation = 0;
    ASSERT_TRUE(queue.pop(frame, generation));
    EXPECT_EQ(generation, 11U);
    // A caller that only accepts generation 12 must reject this frame.
    EXPECT_LT(generation, 12U);
}

TEST(VideoFrameQueueTest, TracksDroppedFramesForDiagnostics) {
    VideoFrameQueue queue(2);
    queue.record_dropped_frame();
    queue.record_dropped_frame();
    EXPECT_EQ(queue.dropped_frames(), 2U);
}

TEST(VideoFrameQueueTest, ZeroCapacityFallsBackToTheDefault) {
    VideoFrameQueue queue(0);
    EXPECT_EQ(queue.capacity(), VideoFrameQueue::kDefaultCapacity);
}

// A source that stopped and started again has to publish frames again
// (BUG-1). Re-opening is the difference between a restarted video and a
// wallpaper that silently stopped moving.
TEST(VideoFrameQueueTest, ReopenAcceptsFramesAgainAfterClose) {
    VideoFrameQueue queue(2);
    ASSERT_TRUE(queue.push(make_frame(1), 1, {}));
    queue.close();
    EXPECT_FALSE(queue.push(make_frame(2), 1, {}));

    queue.reopen();
    EXPECT_TRUE(queue.push(make_frame(3), 1, {}));
    EXPECT_EQ(queue.size(), 1U);

    VideoFrame frame;
    std::uint64_t generation = 0;
    ASSERT_TRUE(queue.pop(frame, generation));
    EXPECT_EQ(pixel_value(frame), 3);
}

// Re-opening a queue that never closed must not drop the frame decoded during
// prepare(): that frame is the first thing a freshly committed source draws.
TEST(VideoFrameQueueTest, ReopenKeepsAnOpenQueueIntact) {
    VideoFrameQueue queue(2);
    ASSERT_TRUE(queue.push(make_frame(4), 1, {}));

    queue.reopen();
    EXPECT_EQ(queue.size(), 1U);
    EXPECT_TRUE(queue.push(make_frame(5), 1, {}));

    VideoFrame frame;
    std::uint64_t generation = 0;
    ASSERT_TRUE(queue.pop(frame, generation));
    EXPECT_EQ(pixel_value(frame), 4);
}
