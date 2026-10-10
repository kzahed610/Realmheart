#include "wallpaper-native/WallpaperScheduling.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>

using namespace std::chrono_literals;
using realmheart::wallpaper_native::kTransitionFrameInterval;
using realmheart::wallpaper_native::wallpaper_poll_timeout_ms;

namespace {

std::chrono::steady_clock::time_point now() {
    return std::chrono::steady_clock::now();
}

} // namespace

// Phase 0 regression: the static idle invariant. Without a transition or a
// timed source the renderer must block indefinitely, never spin on a timer.
TEST(WallpaperSchedulingTest, StableStaticWallpaperBlocksIndefinitely) {
    EXPECT_EQ(
        wallpaper_poll_timeout_ms(false, std::nullopt, now()),
        -1
    );
}

TEST(WallpaperSchedulingTest, TransitionKeepsItsFrameCadence) {
    EXPECT_EQ(
        wallpaper_poll_timeout_ms(true, std::nullopt, now()),
        kTransitionFrameInterval.count()
    );
}

TEST(WallpaperSchedulingTest, QuietTransitionDeadlineWinsOverLaterSourceDeadlines) {
    const auto start = now();
    EXPECT_EQ(
        wallpaper_poll_timeout_ms(true, start + 250ms, start),
        kTransitionFrameInterval.count()
    );
}

TEST(WallpaperSchedulingTest, SourceDeadlineDueSoonerThanTransition) {
    const auto start = now();
    EXPECT_EQ(wallpaper_poll_timeout_ms(true, start + 5ms, start), 5);
}

TEST(WallpaperSchedulingTest, SourceDeadlineReportsRemainingMilliseconds) {
    const auto start = now();
    EXPECT_EQ(wallpaper_poll_timeout_ms(false, start + 41ms, start), 41);
}

TEST(WallpaperSchedulingTest, SourceDeadlineAlreadyDueDoesNotBlock) {
    const auto start = now();
    EXPECT_EQ(wallpaper_poll_timeout_ms(false, start - 1s, start), 0);
    EXPECT_EQ(wallpaper_poll_timeout_ms(false, start, start), 0);
}

TEST(WallpaperSchedulingTest, FarFutureDeadlineStaysInsidePollRange) {
    const auto start = now();
    // ~11 years: the remaining milliseconds overflow int, so the timeout must
    // clamp instead of wrapping into a negative (infinite) poll.
    EXPECT_EQ(
        wallpaper_poll_timeout_ms(false, start + std::chrono::hours(100'000), start),
        std::numeric_limits<int>::max()
    );
}

TEST(WallpaperSchedulingTest, DeadlineOneDayAheadIsReportedInMilliseconds) {
    const auto start = now();
    EXPECT_EQ(wallpaper_poll_timeout_ms(false, start + 24h, start), 86'400'000);
}
