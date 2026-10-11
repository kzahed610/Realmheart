#include "wallpaper-native/video/FrameScheduler.hpp"

#include <algorithm>

namespace realmheart::wallpaper_native {

void FrameScheduler::start(std::chrono::steady_clock::time_point now) noexcept {
    anchor_ = now;
    presented_frames_ = 0;
    running_ = true;
}

void FrameScheduler::restart(std::chrono::steady_clock::time_point now) noexcept {
    anchor_ = now;
    presented_frames_ = 0;
    ++loop_restarts_;
    running_ = true;
}

void FrameScheduler::stop() noexcept {
    running_ = false;
    presented_frames_ = 0;
}

void FrameScheduler::note_consumed(std::size_t count) noexcept {
    presented_frames_ += static_cast<std::uint64_t>(count);
}

std::chrono::steady_clock::duration FrameScheduler::fallback_interval()
    const noexcept {
    if (fallback_fps_ <= 0.0) return {};
    const auto microseconds =
        static_cast<std::int64_t>(1000000.0 / fallback_fps_);
    if (microseconds <= 0) return {};
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::microseconds(microseconds)
    );
}

FrameScheduler::Decision FrameScheduler::decide(
    std::chrono::steady_clock::time_point now,
    std::span<const FrameView> queued
) {
    Decision decision;
    if (!running_ || queued.empty()) {
        // No source, or nothing decoded yet: the renderer has no timed work.
        decision.action = Decision::Action::Idle;
        return decision;
    }

    // A frame that arrived before playback started (a clock step backwards,
    // or a start set slightly in the future) would be permanently late and
    // never presented. Re-anchor instead of wedging the timeline, once per
    // discontinuity.
    if (now < anchor_) {
        anchor_ = now;
        presented_frames_ = 0;
        ++clock_resets_;
    }

    const auto fallback = fallback_interval();
    const auto deadline_for = [&](std::size_t index)
            -> std::chrono::steady_clock::time_point {
        const FrameView& frame = queued[index];
        if (frame.has_timestamp) {
            return anchor_ + std::chrono::microseconds(frame.presentation_us);
        }
        if (fallback == std::chrono::steady_clock::duration::zero()) {
            // No timestamps and no usable rate: this frame can never be due.
            return std::chrono::steady_clock::time_point::max();
        }
        const auto absolute = presented_frames_ + static_cast<std::uint64_t>(index);
        return anchor_ + fallback * absolute;
    };

    // Frames are queued in presentation order, so the newest due frame is the
    // one to show: presenting every missed frame instead would build latency
    // and slow playback down to drain a backlog (renderer docs section 8.3).
    std::size_t newest_due = queued.size();
    while (newest_due > 0) {
        if (deadline_for(newest_due - 1) <= now) break;
        --newest_due;
    }

    if (newest_due == 0) {
        const auto next = deadline_for(0);
        if (next == std::chrono::steady_clock::time_point::max()) {
            // The frame can never become due (no timestamps, no usable rate):
            // report no timed work rather than a far-future deadline that only
            // looks like one.
            decision.action = Decision::Action::Idle;
            return decision;
        }
        decision.action = Decision::Action::Wait;
        decision.deadline = next;
        return decision;
    }

    decision.action = Decision::Action::Present;
    decision.frame_index = newest_due - 1;
    decision.drop_before = newest_due - 1;
    dropped_frames_ += static_cast<std::uint64_t>(newest_due - 1);
    return decision;
}

std::optional<std::chrono::steady_clock::time_point>
FrameScheduler::next_deadline(
    std::chrono::steady_clock::time_point now,
    std::span<const FrameView> queued
) const noexcept {
    if (!running_ || queued.empty()) return std::nullopt;
    if (now < anchor_) return now;

    const auto fallback = fallback_interval();
    const auto deadline_for = [&](std::size_t index)
            -> std::chrono::steady_clock::time_point {
        const FrameView& frame = queued[index];
        if (frame.has_timestamp) {
            return anchor_ + std::chrono::microseconds(frame.presentation_us);
        }
        if (fallback == std::chrono::steady_clock::duration::zero()) {
            return std::chrono::steady_clock::time_point::max();
        }
        const auto absolute = presented_frames_ + static_cast<std::uint64_t>(index);
        return anchor_ + fallback * absolute;
    };

    const auto deadline = deadline_for(0);
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        // Nothing can ever be due: report no timed work so the renderer blocks
        // indefinitely rather than waking on a bogus far-future deadline.
        return std::nullopt;
    }
    return deadline <= now ? now : deadline;
}

} // namespace realmheart::wallpaper_native
