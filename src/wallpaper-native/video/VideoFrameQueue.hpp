#pragma once

#include "wallpaper-native/video/VideoFrame.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stop_token>

namespace realmheart::wallpaper_native {

// Bounded decoded-frame queue between a decode worker and the renderer thread.
//
// The rules from renderer docs section 7.2 are the contract, not a preference:
//   - capacity is fixed (3 frames to start);
//   - the producer waits when the queue is full instead of growing it;
//   - the consumer may drop stale frames when it is behind;
//   - a source replacement (a new generation) releases every queued frame, so
//     a late frame from a discarded source can never become visible.
class VideoFrameQueue {
public:
    explicit VideoFrameQueue(std::size_t capacity = kDefaultCapacity);

    VideoFrameQueue(const VideoFrameQueue&) = delete;
    VideoFrameQueue& operator=(const VideoFrameQueue&) = delete;

    // Blocks while the queue is full. Returns false when it was cancelled
    // through stop_token, when the generation changed, or when the queue was
    // closed; a producer that gets false must discard its frame.
    [[nodiscard]] bool push(
        VideoFrame frame,
        std::uint64_t generation,
        std::stop_token stop_token
    );

    // Consumer side: takes the oldest frame with a matching generation.
    // Returns false when the queue is empty.
    [[nodiscard]] bool pop(VideoFrame& frame, std::uint64_t& generation);

    // Drops queued frames older than 'generation'. A frame is "newer" only
    // when its generation is greater than or equal to the requested one.
    void discard_stale(std::uint64_t generation);

    // Releases every frame and wakes all waiters. Called when a source is
    // discarded or replaced (renderer docs section 13.3).
    void close() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    // Diagnostics for the resource governor: how deep this queue ever got.
    [[nodiscard]] std::size_t high_water() const noexcept { return high_water_; }
    [[nodiscard]] std::size_t dropped_frames() const noexcept {
        return dropped_frames_;
    }

    // Counts a frame the renderer dropped for being late (renderer docs
    // section 8.3). Late frames are discarded, never replayed.
    void record_dropped_frame() noexcept { ++dropped_frames_; }

    static constexpr std::size_t kDefaultCapacity = 3;

private:
    struct Entry {
        VideoFrame frame;
        std::uint64_t generation = 0;
    };

    std::size_t capacity_;
    std::deque<Entry> entries_;
    mutable std::mutex mutex_;
    // condition_variable_any rather than condition_variable so the producer can
    // wait on a std::stop_token (renderer docs section 7.2).
    std::condition_variable_any space_available_;

    std::size_t high_water_ = 0;
    std::size_t dropped_frames_ = 0;
    bool closed_ = false;
};

} // namespace realmheart::wallpaper_native
