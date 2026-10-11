#include "wallpaper-native/video/WallpaperVideoStats.hpp"

#include <gtest/gtest.h>

#include <string>

using realmheart::wallpaper_native::format_wallpaper_video_stats;
using realmheart::wallpaper_native::WallpaperVideoStats;

namespace {

bool contains_all(const std::string& line, const std::vector<std::string>& keys) {
    for (const auto& key : keys) {
        if (line.find(key + "=") == std::string::npos) return false;
    }
    return true;
}

} // namespace

TEST(WallpaperVideoStatsTest, IdleRendererReportsNoTimedWork) {
    const auto line = format_wallpaper_video_stats(WallpaperVideoStats{});
    EXPECT_EQ(line.rfind("STATS", 0), 0U);
    EXPECT_NE(line.find("active_video_sources=0"), std::string::npos);
    EXPECT_NE(line.find("frames_decoded=0"), std::string::npos);
    EXPECT_NE(line.find("source_failed=0"), std::string::npos);
}

TEST(WallpaperVideoStatsTest, EveryCounterIsPresent) {
    // Anything that parses a STATS line depends on the key set, so lock it down.
    const auto line = format_wallpaper_video_stats(WallpaperVideoStats{});
    EXPECT_TRUE(contains_all(line, {
        "active_video_sources",
        "frames_decoded",
        "frames_dropped",
        "queue_high_water",
        "loop_restarts",
        "clock_resets",
        "decode_wakeups",
        "source_failed",
    }));
}

TEST(WallpaperVideoStatsTest, ValuesAreRenderedVerbatim) {
    WallpaperVideoStats stats;
    stats.frames_decoded = 1234;
    stats.frames_dropped = 56;
    stats.loop_restarts = 7;
    stats.clock_resets = 2;
    stats.decode_wakeups = 90;
    stats.queue_high_water = 3;
    stats.active_video_sources = 1;

    const auto line = format_wallpaper_video_stats(stats);
    EXPECT_NE(line.find("frames_decoded=1234"), std::string::npos);
    EXPECT_NE(line.find("frames_dropped=56"), std::string::npos);
    EXPECT_NE(line.find("loop_restarts=7"), std::string::npos);
    EXPECT_NE(line.find("clock_resets=2"), std::string::npos);
    EXPECT_NE(line.find("decode_wakeups=90"), std::string::npos);
    EXPECT_NE(line.find("queue_high_water=3"), std::string::npos);
    EXPECT_NE(line.find("active_video_sources=1"), std::string::npos);
}

TEST(WallpaperVideoStatsTest, FailureIsANumericFlagNotATextFlag) {
    WallpaperVideoStats stats;
    stats.any_source_failed = true;
    EXPECT_NE(
        format_wallpaper_video_stats(stats).find("source_failed=1"),
        std::string::npos
    );
    EXPECT_EQ(
        format_wallpaper_video_stats(WallpaperVideoStats{})
            .find("source_failed=1"),
        std::string::npos
    );
}

// A frame dropped because it was late is the counter that reveals a queue that
// cannot keep up, so it must survive aggregation rather than being folded away.
TEST(WallpaperVideoStatsTest, AggregationKeepsDropsVisible) {
    WallpaperVideoStats total;
    WallpaperVideoStats first;
    first.frames_decoded = 40;
    first.frames_dropped = 3;
    total.frames_decoded += first.frames_decoded;
    total.frames_dropped += first.frames_dropped;

    EXPECT_EQ(total.frames_dropped, 3U);
    EXPECT_NE(
        format_wallpaper_video_stats(total).find("frames_dropped=3"),
        std::string::npos
    );
}
