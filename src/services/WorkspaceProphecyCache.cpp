#include "services/WorkspaceProphecyCache.hpp"

#include <algorithm>
#include <cstdlib>

namespace realmheart::services {

void WorkspaceProphecyCache::record_visit(int workspace_id, std::string workspace_name) {
    if (workspace_id <= 0) return;

    auto it = index_by_id_.find(workspace_id);
    if (it != index_by_id_.end()) {
        // Move existing entry to front (most recent).
        const std::size_t old_index = it->second;
        CacheEntry entry = std::move(mru_order_[old_index]);

        // Shift everything before old_index down by one.
        for (std::size_t i = old_index; i > 0; --i) {
            mru_order_[i] = std::move(mru_order_[i - 1]);
        }
        mru_order_[0] = std::move(entry);

        // Rebuild index map.
        index_by_id_.clear();
        for (std::size_t i = 0; i < mru_order_.size(); ++i) {
            index_by_id_[mru_order_[i].workspace_id] = i;
        }
        return;
    }

    // New entry — evict oldest if at capacity.
    if (mru_order_.size() >= kMaxCacheSize) {
        const int evicted_id = mru_order_.back().workspace_id;
        index_by_id_.erase(evicted_id);
        mru_order_.pop_back();
    }

    // Prepend as most recent.
    mru_order_.insert(mru_order_.begin(), CacheEntry{
        .workspace_id = workspace_id,
        .workspace_name = std::move(workspace_name),
    });

    index_by_id_.clear();
    for (std::size_t i = 0; i < mru_order_.size(); ++i) {
        index_by_id_[mru_order_[i].workspace_id] = i;
    }
}

void WorkspaceProphecyCache::clear() {
    mru_order_.clear();
    index_by_id_.clear();
}

std::optional<WorkspaceProphecyCache::Selection>
WorkspaceProphecyCache::select_futures(int active_workspace_id, std::uint64_t seed) const {
    if (mru_order_.empty()) return std::nullopt;

    Selection selection;
    selection.seed = seed;
    selection.active_workspace_id = active_workspace_id;

    // The active workspace is always the dominant future (position 0).
    // Find it in the cache; if not found, still make it dominant from
    // what we know.
    bool active_found = false;
    for (const auto& entry : mru_order_) {
        if (entry.workspace_id == active_workspace_id) {
            selection.futures.push_back(Future{
                .workspace_id = entry.workspace_id,
                .workspace_name = entry.workspace_name,
                .is_active = true,
                .is_dominant = true,
            });
            active_found = true;
            break;
        }
    }

    if (!active_found) {
        // Active workspace not in cache — use a placeholder. This can happen
        // on first lock if the active workspace was just visited.
        selection.futures.push_back(Future{
            .workspace_id = active_workspace_id,
            .workspace_name = std::to_string(active_workspace_id),
            .is_active = true,
            .is_dominant = true,
        });
    }

    // Fill remaining futures from MRU order, skipping the active one.
    for (const auto& entry : mru_order_) {
        if (entry.workspace_id == active_workspace_id) continue;
        if (selection.futures.size() >= kMaxFutures) break;

        selection.futures.push_back(Future{
            .workspace_id = entry.workspace_id,
            .workspace_name = entry.workspace_name,
            .is_active = false,
            .is_dominant = false,
        });
    }

    // If we have fewer than kMinFutures entries, we accept fewer futures.
    // The layout engine will fill missing slots with wallpaper-only placeholders.
    return selection;
}

bool WorkspaceProphecyCache::contains(int workspace_id) const noexcept {
    return index_by_id_.contains(workspace_id);
}

} // namespace realmheart::services
