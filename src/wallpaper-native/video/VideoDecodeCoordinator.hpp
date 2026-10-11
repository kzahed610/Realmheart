#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace realmheart::wallpaper_native {

// The single wakeup channel between the video decode workers and the renderer
// loop.
//
// One eventfd serves every active video source: a worker signals it when it
// publishes a frame, loops, or fails, and the renderer adds it to its poll set.
// Keeping that in one place is what stops a per-video-source poll entry from
// growing into a per-monitor argument (renderer docs section 7.3), and it keeps
// the renderer from polling decoders at a fixed interval.
class VideoDecodeCoordinator {
public:
    VideoDecodeCoordinator() = default;
    ~VideoDecodeCoordinator();

    VideoDecodeCoordinator(const VideoDecodeCoordinator&) = delete;
    VideoDecodeCoordinator& operator=(const VideoDecodeCoordinator&) = delete;

    // Creates the eventfd. Must succeed before any worker runs.
    [[nodiscard]] bool initialize(std::string* error_message = nullptr);
    void shutdown() noexcept;

    [[nodiscard]] int wake_fd() const noexcept { return wake_fd_; }
    [[nodiscard]] bool ready() const noexcept { return wake_fd_ >= 0; }

    // Worker side: makes the channel readable so the renderer wakes. Safe to
    // call after shutdown.
    void wake() noexcept;

    // Renderer side: drains the counter so the next signal is a new edge.
    void acknowledge() noexcept;

    [[nodiscard]] std::uint64_t wakeups() const noexcept {
        return wakeups_.load(std::memory_order_relaxed);
    }

private:
    int wake_fd_ = -1;
    // Signalled from worker threads and read from the renderer thread.
    std::atomic<std::uint64_t> wakeups_{0};
};

} // namespace realmheart::wallpaper_native
