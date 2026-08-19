#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace realmheart::services {

// Bounded MRU cache of recently-visited workspaces, plus the pure-logic
// selection algorithm that chooses which workspaces become "futures" on the
// lock screen. Keeps no image data — only metadata; images are captured
// separately by ProphecySnapshotStore.
class WorkspaceProphecyCache {
public:
    static constexpr std::size_t kMaxCacheSize = 8;
    static constexpr std::size_t kMinFutures = 4;
    static constexpr std::size_t kMaxFutures = 6;

    struct Future {
        int workspace_id = 0;
        std::string workspace_name;
        bool is_active = false;
        bool is_dominant = false;
    };

    struct Selection {
        std::uint64_t seed = 0;
        int active_workspace_id = 0;
        std::vector<Future> futures;
    };

    WorkspaceProphecyCache() = default;

    // Record that a workspace was visited (activated). Maintains MRU order.
    // The active workspace at lock time is always position 0.
    void record_visit(int workspace_id, std::string workspace_name);

    // Clear all cached entries.
    void clear();

    // Select which workspaces to show as futures on the lock screen.
    // Always includes the currently active workspace as the dominant future
    // (position 0 in the output), then fills from MRU order.
    // Returns nullopt if no workspaces have been recorded.
    [[nodiscard]] std::optional<Selection> select_futures(
        int active_workspace_id,
        std::uint64_t seed
    ) const;

    // Number of workspaces currently in the cache.
    [[nodiscard]] std::size_t size() const noexcept { return mru_order_.size(); }

    // True if the given workspace is in the cache.
    [[nodiscard]] bool contains(int workspace_id) const noexcept;

private:
    struct CacheEntry {
        int workspace_id = 0;
        std::string workspace_name;
    };

    // mru_order_[0] is most-recently-visited.
    std::vector<CacheEntry> mru_order_;

    // Quick lookup for contains/eviction.
    std::unordered_map<int, std::size_t> index_by_id_;
};

} // namespace realmheart::services
