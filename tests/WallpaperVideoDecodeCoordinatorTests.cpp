#include "wallpaper-native/video/VideoDecodeCoordinator.hpp"

#include <gtest/gtest.h>

#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <thread>

using realmheart::wallpaper_native::VideoDecodeCoordinator;

namespace {

bool readable(int descriptor, int timeout_ms) {
    pollfd entry{descriptor, POLLIN, 0};
    return ::poll(&entry, 1, timeout_ms) == 1 &&
           (entry.revents & POLLIN) != 0;
}

} // namespace

TEST(VideoDecodeCoordinatorTest, InitializeExposesAValidDescriptor) {
    VideoDecodeCoordinator coordinator;
    std::string error;
    ASSERT_TRUE(coordinator.initialize(&error)) << error;
    EXPECT_TRUE(coordinator.ready());
    EXPECT_GE(coordinator.wake_fd(), 0);
    // Nothing signalled yet, so it must not be readable.
    EXPECT_FALSE(readable(coordinator.wake_fd(), 20));
}

TEST(VideoDecodeCoordinatorTest, WakeMakesTheChannelReadable) {
    VideoDecodeCoordinator coordinator;
    ASSERT_TRUE(coordinator.initialize());

    coordinator.wake();
    EXPECT_TRUE(readable(coordinator.wake_fd(), 1000));
    EXPECT_EQ(coordinator.wakeups(), 1U);
}

// A drain must leave the channel quiet: otherwise every wake would look like a
// new one and the renderer would spin.
TEST(VideoDecodeCoordinatorTest, AcknowledgeDrainsTheSignal) {
    VideoDecodeCoordinator coordinator;
    ASSERT_TRUE(coordinator.initialize());

    coordinator.wake();
    coordinator.acknowledge();
    EXPECT_FALSE(readable(coordinator.wake_fd(), 100));

    // A second signal is a fresh edge.
    coordinator.wake();
    EXPECT_TRUE(readable(coordinator.wake_fd(), 1000));
    coordinator.acknowledge();
    EXPECT_FALSE(readable(coordinator.wake_fd(), 100));
}

// Many wakes must collapse into one edge rather than an ever-growing counter.
TEST(VideoDecodeCoordinatorTest, RepeatedWakesCollapseAndDrainCleanly) {
    VideoDecodeCoordinator coordinator;
    ASSERT_TRUE(coordinator.initialize());

    for (int index = 0; index < 50; ++index) coordinator.wake();
    EXPECT_EQ(coordinator.wakeups(), 50U);
    EXPECT_TRUE(readable(coordinator.wake_fd(), 1000));
    coordinator.acknowledge();
    EXPECT_FALSE(readable(coordinator.wake_fd(), 100));
}

// Signalling from several threads at once must never lose the wakeup.
TEST(VideoDecodeCoordinatorTest, ConcurrentWakesAreNotLost) {
    VideoDecodeCoordinator coordinator;
    ASSERT_TRUE(coordinator.initialize());

    std::jthread first([&coordinator] {
        for (int index = 0; index < 200; ++index) coordinator.wake();
    });
    std::jthread second([&coordinator] {
        for (int index = 0; index < 200; ++index) coordinator.wake();
    });
    first.join();
    second.join();

    EXPECT_EQ(coordinator.wakeups(), 400U);
    EXPECT_TRUE(readable(coordinator.wake_fd(), 1000));
    coordinator.acknowledge();
    EXPECT_FALSE(readable(coordinator.wake_fd(), 100));
}

TEST(VideoDecodeCoordinatorTest, ShutdownClosesTheDescriptorAndSurvivesWakes) {
    VideoDecodeCoordinator coordinator;
    ASSERT_TRUE(coordinator.initialize());
    const int descriptor = coordinator.wake_fd();

    coordinator.shutdown();
    EXPECT_FALSE(coordinator.ready());
    EXPECT_EQ(coordinator.wake_fd(), -1);
    // A worker still running during teardown must not crash the renderer.
    EXPECT_NO_THROW(coordinator.wake());
    EXPECT_NO_THROW(coordinator.acknowledge());

    // The descriptor is really gone: polling it reports an error condition,
    // never readability. (Its number may be handed to a later eventfd, which
    // is fine; the closed one must simply not be readable.)
    pollfd entry{descriptor, POLLIN, 0};
    (void)::poll(&entry, 1, 20);
    EXPECT_EQ(entry.revents & POLLIN, 0);
}

TEST(VideoDecodeCoordinatorTest, InitializeTwiceLeavesAWorkingChannel) {
    VideoDecodeCoordinator coordinator;
    ASSERT_TRUE(coordinator.initialize());
    coordinator.wake();
    coordinator.acknowledge();

    std::string error;
    ASSERT_TRUE(coordinator.initialize(&error)) << error;
    EXPECT_TRUE(coordinator.ready());
    EXPECT_GE(coordinator.wake_fd(), 0);
    // The previous signal was drained with the old channel, so the new one
    // starts quiet and works.
    EXPECT_FALSE(readable(coordinator.wake_fd(), 50));
    coordinator.wake();
    EXPECT_TRUE(readable(coordinator.wake_fd(), 1000));
}

TEST(VideoDecodeCoordinatorTest, UninitializedCoordinatorIsInert) {
    VideoDecodeCoordinator coordinator;
    EXPECT_FALSE(coordinator.ready());
    EXPECT_NO_THROW(coordinator.wake());
    EXPECT_NO_THROW(coordinator.acknowledge());
}
