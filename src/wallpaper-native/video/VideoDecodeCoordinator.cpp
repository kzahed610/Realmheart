#include "wallpaper-native/video/VideoDecodeCoordinator.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>

#include <cerrno>
#include <cstdint>

namespace realmheart::wallpaper_native {

VideoDecodeCoordinator::~VideoDecodeCoordinator() {
    shutdown();
}

bool VideoDecodeCoordinator::initialize(std::string* error_message) {
    shutdown();

    const int descriptor = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (descriptor < 0) {
        if (error_message != nullptr) {
            *error_message = "unable to create the video decode wakeup channel";
        }
        return false;
    }
    wake_fd_ = descriptor;
    wakeups_ = 0;
    return true;
}

void VideoDecodeCoordinator::shutdown() noexcept {
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
}

void VideoDecodeCoordinator::wake() noexcept {
    if (wake_fd_ < 0) return;
    const std::uint64_t signal = 1;
    // A full counter is fine: the renderer only needs one edge to know a worker
    // wants attention, and a non-blocking write must never stall decoding.
    for (;;) {
        const ssize_t written =
            ::write(wake_fd_, &signal, sizeof(signal));
        if (written == static_cast<ssize_t>(sizeof(signal))) {
            wakeups_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (written < 0 && errno == EINTR) continue;
        return;
    }
}

void VideoDecodeCoordinator::acknowledge() noexcept {
    if (wake_fd_ < 0) return;
    std::uint64_t drained = 0;
    for (;;) {
        const ssize_t read = ::read(wake_fd_, &drained, sizeof(drained));
        if (read == static_cast<ssize_t>(sizeof(drained))) return;
        if (read < 0 && errno == EINTR) continue;
        return;
    }
}

} // namespace realmheart::wallpaper_native
