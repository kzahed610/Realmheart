#include "wallpaper-native/video/VideoFrameQueue.hpp"

#include <algorithm>

namespace realmheart::wallpaper_native {

VideoFrameQueue::VideoFrameQueue(std::size_t capacity)
    : capacity_(capacity == 0 ? kDefaultCapacity : capacity) {}

bool VideoFrameQueue::push(
    VideoFrame frame,
    std::uint64_t generation,
    std::stop_token stop_token
) {
    std::unique_lock lock(mutex_);
    // A full queue is a backstop, not a buffer to grow: waiting here is what
    // keeps a slow renderer from turning into unbounded decoded memory.
    const bool accepted = space_available_.wait(
        lock,
        stop_token,
        [this] { return closed_ || entries_.size() < capacity_; }
    );
    if (!accepted || closed_) return false;

    entries_.push_back(Entry{std::move(frame), generation});
    high_water_ = std::max(high_water_, entries_.size());
    return true;
}

bool VideoFrameQueue::pop(VideoFrame& frame, std::uint64_t& generation) {
    std::lock_guard lock(mutex_);
    if (entries_.empty()) return false;

    // The oldest frame is the one due next. Its generation travels with it so
    // the renderer can reject a frame a discarded source published too late
    // (renderer docs section 13.3); bulk purging is discard_stale().
    Entry entry = std::move(entries_.front());
    entries_.pop_front();
    space_available_.notify_one();

    frame = std::move(entry.frame);
    generation = entry.generation;
    return true;
}

void VideoFrameQueue::discard_stale(std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    const auto before = entries_.size();
    entries_.erase(
        std::remove_if(
            entries_.begin(),
            entries_.end(),
            [generation](const Entry& entry) {
                return entry.generation < generation;
            }
        ),
        entries_.end()
    );
    dropped_frames_ += before - entries_.size();
    space_available_.notify_all();
}

void VideoFrameQueue::close() noexcept {
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        entries_.clear();
    }
    space_available_.notify_all();
}

std::size_t VideoFrameQueue::size() const noexcept {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

} // namespace realmheart::wallpaper_native
