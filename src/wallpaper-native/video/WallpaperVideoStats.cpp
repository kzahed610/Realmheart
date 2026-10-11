#include "wallpaper-native/video/WallpaperVideoStats.hpp"

#include <sstream>

namespace realmheart::wallpaper_native {

std::string format_wallpaper_video_stats(const WallpaperVideoStats& stats) {
    std::ostringstream line;
    line << "STATS"
         << " active_video_sources=" << stats.active_video_sources
         << " frames_decoded=" << stats.frames_decoded
         << " frames_dropped=" << stats.frames_dropped
         << " queue_high_water=" << stats.queue_high_water
         << " loop_restarts=" << stats.loop_restarts
         << " clock_resets=" << stats.clock_resets
         << " decode_wakeups=" << stats.decode_wakeups
         << " source_failed=" << (stats.any_source_failed ? 1 : 0);
    return line.str();
}

} // namespace realmheart::wallpaper_native
