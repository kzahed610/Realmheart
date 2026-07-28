#include "effects/core/TransitionTimeline.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::effects {

namespace {

constexpr double kCompletionEpsilon = 0.000001;

} // namespace

TransitionTimeline::TransitionTimeline(TransitionDurations durations)
    : durations_{
          sanitize_duration(durations.open_seconds),
          sanitize_duration(durations.close_seconds),
      } {}

void TransitionTimeline::open() noexcept {
    if (progress_ >= 1.0 - kCompletionEpsilon || durations_.open_seconds == 0.0) {
        snap_visible();
        return;
    }
    state_ = TransitionState::Opening;
}

void TransitionTimeline::close() noexcept {
    if (progress_ <= kCompletionEpsilon || durations_.close_seconds == 0.0) {
        snap_hidden();
        return;
    }
    state_ = TransitionState::Closing;
}

void TransitionTimeline::toggle() noexcept {
    if (target_visible()) {
        close();
    } else {
        open();
    }
}

void TransitionTimeline::snap_hidden() noexcept {
    progress_ = 0.0;
    state_ = TransitionState::Hidden;
}

void TransitionTimeline::snap_visible() noexcept {
    progress_ = 1.0;
    state_ = TransitionState::Visible;
}

bool TransitionTimeline::advance(double elapsed_seconds) noexcept {
    if (!active()) return false;
    if (!std::isfinite(elapsed_seconds) || elapsed_seconds <= 0.0) return true;

    if (state_ == TransitionState::Opening) {
        progress_ = std::min(
            1.0,
            progress_ + elapsed_seconds / durations_.open_seconds
        );
        if (progress_ >= 1.0 - kCompletionEpsilon) {
            snap_visible();
            return false;
        }
        return true;
    }

    progress_ = std::max(
        0.0,
        progress_ - elapsed_seconds / durations_.close_seconds
    );
    if (progress_ <= kCompletionEpsilon) {
        snap_hidden();
        return false;
    }
    return true;
}

TransitionState TransitionTimeline::state() const noexcept {
    return state_;
}

double TransitionTimeline::progress() const noexcept {
    return progress_;
}

bool TransitionTimeline::active() const noexcept {
    return state_ == TransitionState::Opening ||
        state_ == TransitionState::Closing;
}

bool TransitionTimeline::target_visible() const noexcept {
    return state_ == TransitionState::Opening ||
        state_ == TransitionState::Visible;
}

TransitionDurations TransitionTimeline::durations() const noexcept {
    return durations_;
}

double TransitionTimeline::sanitize_duration(double seconds) noexcept {
    if (!std::isfinite(seconds) || seconds <= 0.0) return 0.0;
    return seconds;
}

} // namespace realmheart::effects
