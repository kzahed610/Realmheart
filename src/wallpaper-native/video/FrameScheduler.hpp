#pragma once

#include "wallpaper-native/video/VideoFrame.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace realmheart::wallpaper_native {

// Turns decoded frame timestamps into wall-clock decisions.
//
// This is the class that keeps the renderer deadline-driven: a 24 FPS video
// produces about 24 presentation deadlines per second, never a monitor-shaped
// loop (renderer docs section 8). It is deliberately free of FFmpeg, Wayland
// and GL so the entire scheduling policy can be tested exactly.
//
// The renderer owns the frame queue; this class only decides what to do with
// the frames it is shown, in the order it is shown them.
class FrameScheduler {
public:
    // A queued frame as the scheduler sees it.
    struct FrameView {
        std::int64_t presentation_us = 0;
        bool has_timestamp = false;
    };

    struct Decision {
        enum class Action {
            // Present queued[frame_index] and discard the 'drop_before' frames
            // ahead of it.
            Present,
            // Nothing is due yet: sleep until 'deadline'.
            Wait,
            // No timed work at all.
            Idle,
        };

        Action action = Action::Idle;
        std::size_t frame_index = 0;
        std::size_t drop_before = 0;
        std::chrono::steady_clock::time_point deadline{};
    };

    // Establishes playback time zero. Called when the source becomes current.
    void start(std::chrono::steady_clock::time_point now) noexcept;

    // Re-anchors the timeline, used when the media loops: the next frame is
    // due one frame interval from 'now', not from the old timeline.
    void restart(std::chrono::steady_clock::time_point now) noexcept;

    // Drops the timeline entirely: no deadlines, no timed work.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept { return running_; }

    // Result of a scheduling pass over the frames currently queued.
    [[nodiscard]] Decision decide(
        std::chrono::steady_clock::time_point now,
        std::span<const FrameView> queued
    );

    // The next moment this source needs the renderer to wake, if any.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_deadline(
        std::chrono::steady_clock::time_point now,
        std::span<const FrameView> queued
    ) const noexcept;

    // Reports how many frames the caller actually removed, so the fallback
    // timeline (media without usable timestamps) stays in step.
    void note_consumed(std::size_t count) noexcept;

    [[nodiscard]] std::uint64_t dropped_frames() const noexcept {
        return dropped_frames_;
    }
    [[nodiscard]] std::uint64_t loop_restarts() const noexcept {
        return loop_restarts_;
    }
    [[nodiscard]] std::uint64_t clock_resets() const noexcept {
        return clock_resets_;
    }
    void set_frames_per_second(double frames_per_second) noexcept {
        fallback_fps_ = frames_per_second > 0.0 ? frames_per_second : 0.0;
    }
    [[nodiscard]] double frames_per_second() const noexcept {
        return fallback_fps_;
    }

private:
    [[nodiscard]] std::chrono::steady_clock::duration fallback_interval() const
        noexcept;

    std::chrono::steady_clock::time_point anchor_{};
    std::uint64_t presented_frames_ = 0;
    std::uint64_t dropped_frames_ = 0;
    std::uint64_t loop_restarts_ = 0;
    std::uint64_t clock_resets_ = 0;
    double fallback_fps_ = 0.0;
    bool running_ = false;
};

} // namespace realmheart::wallpaper_native
