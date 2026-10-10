#include "wallpaper-native/source/VideoWallpaperSource.hpp"

namespace realmheart::wallpaper_native {

bool VideoWallpaperSource::prepare(std::string* error_message) {
    if (error_message != nullptr) {
        *error_message =
            "live video wallpaper playback is not implemented yet; this build "
            "classifies video sources but cannot decode them";
    }
    // A video that cannot be prepared must never become current, so the
    // transaction fails and the previous wallpaper stays untouched.
    return false;
}

void VideoWallpaperSource::start(std::chrono::steady_clock::time_point) noexcept {
    // No playback clock exists yet.
}

void VideoWallpaperSource::stop() noexcept {}

SourceTickResult VideoWallpaperSource::tick(
    std::chrono::steady_clock::time_point
) noexcept {
    // No decoded frame can be published, so nothing forces a redraw.
    return {};
}

RenderFrameView VideoWallpaperSource::current_frame() const noexcept {
    return RenderFrameView{};
}

void VideoWallpaperSource::set_output_requirement(
    int /*width*/,
    int /*height*/
) noexcept {}

bool VideoWallpaperSource::requires_topology_rederive(
    int /*required_width*/,
    int /*required_height*/
) const noexcept {
    // Video frames keep their encoded geometry: a topology change never
    // re-derives them, it only changes the crop.
    return false;
}

bool VideoWallpaperSource::adapt_to_topology(
    int /*required_width*/,
    int /*required_height*/,
    std::string* /*error_message*/
) {
    return true;
}

RuntimeSourcePtr make_video_wallpaper_source(
    std::string path,
    std::string detail
) {
    return std::make_shared<VideoWallpaperSource>(std::move(path), std::move(detail));
}

} // namespace realmheart::wallpaper_native
