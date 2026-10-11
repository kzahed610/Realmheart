#pragma once

#include "wallpaper-native/source/RuntimeWallpaperSource.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace realmheart::wallpaper_native {

// The live (video) wallpaper source.
//
// This patch lands the seam, not the player: a source is classified as video,
// carries its probe detail, and fails prepare() with a specific message rather
// than pretending a frozen frame or an error log means playback succeeded.
// The decoder, its bounded queue and the presentation scheduler that feed this
// source arrive in the following patches.
class VideoWallpaperSource : public RuntimeWallpaperSource {
public:
    VideoWallpaperSource(std::string path, std::string detail)
        : path_(std::move(path)), detail_(std::move(detail)) {}

    [[nodiscard]] WallpaperMediaKind kind() const noexcept override {
        return WallpaperMediaKind::Video;
    }

    [[nodiscard]] bool prepare(std::string* error_message) override;
    void start(std::chrono::steady_clock::time_point now) noexcept override;
    void stop() noexcept override;
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
    next_deadline() const noexcept override;
    SourceTickResult tick(std::chrono::steady_clock::time_point now) noexcept override;
    [[nodiscard]] RenderFrameView current_frame() const noexcept override;
    [[nodiscard]] bool has_frame() const noexcept override {
        return false;
    }
    void set_output_requirement(int width, int height) noexcept override;
    [[nodiscard]] bool requires_topology_rederive(
        int required_width,
        int required_height
    ) const noexcept override;
    [[nodiscard]] bool adapt_to_topology(
        int required_width,
        int required_height,
        std::string* error_message
    ) override;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

private:
    std::string path_;
    std::string detail_;
};

[[nodiscard]] RuntimeSourcePtr make_video_wallpaper_source(
    std::string path,
    std::string detail = {}
);

} // namespace realmheart::wallpaper_native
