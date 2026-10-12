#pragma once

#include "wallpaper-native/gpu/CpuUploadFrameImporter.hpp"
#include "wallpaper-native/gpu/RenderFrame.hpp"
#include "wallpaper-native/source/RuntimeWallpaperSource.hpp"
#include "wallpaper-native/video/FrameScheduler.hpp"
#include "wallpaper-native/video/VideoDecodeCoordinator.hpp"
#include "wallpaper-native/video/VideoDecoder.hpp"
#include "wallpaper-native/video/VideoFrame.hpp"
#include "wallpaper-native/video/VideoFrameQueue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace realmheart::wallpaper_native {

// The live (video) wallpaper source.
//
// A video source owns its decode timeline: the FFmpeg decoder runs on a worker
// thread that pushes decoded frames into a bounded queue, and the renderer
// thread presents the frame whose presentation deadline has arrived. Nothing
// FFmpeg-shaped and no decoded pixel ever crosses into the renderer without
// going through this class, and the worker never touches GL.
//
// Destruction requires a current GL context, because releasing the last
// reference deletes the uploaded texture.
class VideoWallpaperSource : public RuntimeWallpaperSource {
public:
    VideoWallpaperSource(
        std::string path,
        std::string detail,
        VideoDecodeCoordinator* coordinator
    );
    ~VideoWallpaperSource() override;

    VideoWallpaperSource(const VideoWallpaperSource&) = delete;
    VideoWallpaperSource& operator=(const VideoWallpaperSource&) = delete;

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
    [[nodiscard]] bool has_frame() const noexcept override;
    [[nodiscard]] bool committable() const noexcept override;
    [[nodiscard]] bool playing() const noexcept override { return started_; }
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

    // Backend counters for the resource governor (renderer docs section 23).
    // Reading them touches only atomics, so they are safe from any thread.
    [[nodiscard]] std::uint64_t frames_decoded() const noexcept {
        return frames_decoded_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t frames_dropped() const noexcept {
        return frames_dropped_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t loop_restarts() const noexcept {
        return scheduler_.loop_restarts();
    }
    [[nodiscard]] std::uint64_t clock_resets() const noexcept {
        return scheduler_.clock_resets();
    }
    [[nodiscard]] std::size_t queue_high_water() const noexcept {
        return queue_.high_water();
    }
    [[nodiscard]] bool failed() const noexcept {
        return failed_.load(std::memory_order_relaxed);
    }

    // Lifecycle state for the regression tests around BUG-1 (re-applying the
    // wallpaper that is already playing). Reading them never touches the
    // worker thread's FFmpeg state.
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool decoder_open() const noexcept { return decoder_.is_open(); }
    [[nodiscard]] std::size_t queue_size() const noexcept { return queue_.size(); }

    void collect_stats(WallpaperVideoStats& stats) const noexcept override {
        stats.frames_decoded += frames_decoded();
        stats.frames_dropped += frames_dropped();
        stats.loop_restarts += loop_restarts();
        stats.clock_resets += clock_resets();
        stats.queue_high_water =
            std::max(stats.queue_high_water, queue_high_water());
        ++stats.active_video_sources;
        if (failed()) stats.any_source_failed = true;
    }

private:
    void decode_loop(std::stop_token token);
    void freeze(std::string_view reason) noexcept;

    std::string path_;
    std::string detail_;
    VideoDecoder decoder_;
    VideoFrameQueue queue_;
    FrameScheduler scheduler_;
    CpuUploadFrameImporter importer_;
    VideoDecodeCoordinator* coordinator_ = nullptr;

    RenderFrame frame_{};
    std::uint64_t generation_ = 0;
    std::atomic<bool> loop_pending_{false};
    std::atomic<bool> failed_{false};
    bool prepared_ = false;
    bool started_ = false;
    std::atomic<std::uint64_t> frames_decoded_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};

    // Declared last: the thread is stopped and joined before the members it
    // touches are destroyed.
    std::jthread worker_;
};

[[nodiscard]] RuntimeSourcePtr make_video_wallpaper_source(
    std::string path,
    std::string detail,
    VideoDecodeCoordinator* coordinator
);

} // namespace realmheart::wallpaper_native
