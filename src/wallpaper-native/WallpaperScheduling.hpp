#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace realmheart::wallpaper_native {

// The native renderer is deadline-driven and idle by default: a stable static
// wallpaper must block in poll() indefinitely and perform no periodic work.
// Transitions keep a ~16 ms visual cadence until they are synchronized to
// compositor frame callbacks; every other timed source (video PTS deadlines)
// is expressed as an absolute steady_clock deadline.
inline constexpr std::chrono::milliseconds kTransitionFrameInterval{16};

// Maps the renderer's timed work onto a poll() timeout.
//
// Returns -1 when nothing timed is pending (block indefinitely), 0 when a
// deadline is already due, and otherwise the milliseconds remaining until the
// earliest deadline.
[[nodiscard]] inline int wallpaper_poll_timeout_ms(
    bool transition_active,
    std::optional<std::chrono::steady_clock::time_point> source_deadline,
    std::chrono::steady_clock::time_point now
) noexcept {
    constexpr int kNoTimeout = -1;
    const auto clamp_ms = [](std::int64_t milliseconds) {
        return static_cast<int>(
            std::clamp<std::int64_t>(milliseconds, 0, std::numeric_limits<int>::max())
        );
    };

    int timeout = kNoTimeout;
    if (transition_active) {
        timeout = clamp_ms(kTransitionFrameInterval.count());
    }

    if (source_deadline.has_value()) {
        const auto remaining = *source_deadline - now;
        const bool already_due = remaining <= remaining.zero();
        const int source_timeout = already_due
            ? 0
            : clamp_ms(
                  std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count()
              );
        timeout = timeout == kNoTimeout ? source_timeout
                                        : std::min(timeout, source_timeout);
    }

    return timeout;
}

} // namespace realmheart::wallpaper_native
