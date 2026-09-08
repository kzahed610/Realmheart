#pragma once

#include "effects/core/TransitionTimeline.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace realmheart::ui::workspace {

inline constexpr std::uint32_t kWorkspaceOverviewAssetMaxAttempts = 5;
inline constexpr std::int64_t kWorkspaceOverviewAssetInitialBackoffUs = 50'000;
inline constexpr std::int64_t kWorkspaceOverviewAssetMaxBackoffUs = 2'000'000;
inline constexpr std::size_t kWorkspaceOverviewIconSurfaceCacheLimit = 128;

struct WorkspaceOverviewAssetRetryPolicy {
    std::uint32_t failures = 0;
    std::int64_t next_retry_us = 0;
    bool exhausted = false;

    [[nodiscard]] bool can_attempt(std::int64_t now_us) const noexcept {
        return !exhausted && now_us >= next_retry_us;
    }

    void record_failure(std::int64_t now_us) noexcept {
        if (exhausted) return;
        ++failures;
        const auto shift = std::min<std::uint32_t>(failures - 1U, 5U);
        const auto backoff = std::min<std::int64_t>(
            kWorkspaceOverviewAssetInitialBackoffUs << shift,
            kWorkspaceOverviewAssetMaxBackoffUs
        );
        next_retry_us = now_us > INT64_MAX - backoff
            ? INT64_MAX
            : now_us + backoff;
        exhausted = failures >= kWorkspaceOverviewAssetMaxAttempts;
    }

    void reset() noexcept {
        failures = 0;
        next_retry_us = 0;
        exhausted = false;
    }
};

[[nodiscard]] inline bool workspace_overview_icon_cache_needs_eviction(
    std::size_t current_size
) noexcept {
    return current_size >= kWorkspaceOverviewIconSurfaceCacheLimit;
}

[[nodiscard]] inline bool workspace_overview_defers_snapshot_update(
    effects::TransitionState state
) noexcept {
    return state == effects::TransitionState::Closing;
}

} // namespace realmheart::ui::workspace
