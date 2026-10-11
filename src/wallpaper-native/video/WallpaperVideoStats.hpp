#pragma once

#include "wallpaper-native/video/VideoFrame.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace realmheart::wallpaper_native {

// Aggregated live-wallpaper counters for the resource governor and Doctor
// (renderer docs sections 16 and 23). These exist so a performance regression
// can be diagnosed without attaching a profiler first.
struct WallpaperVideoStats {
    std::uint64_t frames_decoded = 0;
    std::uint64_t frames_dropped = 0;
    std::uint64_t loop_restarts = 0;
    std::uint64_t clock_resets = 0;
    std::uint64_t decode_wakeups = 0;
    // Deepest any queue ran, across every live source.
    std::size_t queue_high_water = 0;
    std::size_t active_video_sources = 0;
    bool any_source_failed = false;
};

// Renders the counters as a single stable line. Key order and spelling are part
// of the contract: anything parsing a STATS response depends on them, so keep
// this pure and test it when the shape changes.
[[nodiscard]] std::string format_wallpaper_video_stats(
    const WallpaperVideoStats& stats
);

} // namespace realmheart::wallpaper_native
