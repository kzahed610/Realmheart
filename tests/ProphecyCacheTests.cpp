#include "services/WorkspaceProphecyCache.hpp"

#include <cstdlib>
#include <cassert>
#include <iostream>
#include <string>

namespace {

void require(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

void test_empty_cache_returns_nullopt() {
    realmheart::services::WorkspaceProphecyCache cache;
    auto result = cache.select_futures(1, 42);
    require(!result.has_value(), "empty cache must return nullopt");
    require(cache.size() == 0, "empty cache size must be 0");
    require(!cache.contains(1), "empty cache must not contain any workspace");
}

void test_single_visit_dominant_future() {
    realmheart::services::WorkspaceProphecyCache cache;
    cache.record_visit(3, "code");

    auto result = cache.select_futures(3, 42);
    require(result.has_value(), "single visit must return selection");
    require(result->futures.size() == 1, "single visit must yield 1 future");

    const auto& f = result->futures[0];
    require(f.workspace_id == 3, "dominant future must be workspace 3");
    require(f.is_active, "dominant future must be active");
    require(f.is_dominant, "dominant future must be marked dominant");
    require(f.workspace_name == "code", "workspace name must match");
    require(result->seed == 42, "seed must be preserved");
    require(result->active_workspace_id == 3, "active id must be preserved");
}

void test_mru_ordering_active_at_front() {
    realmheart::services::WorkspaceProphecyCache cache;
    cache.record_visit(1, "web");
    cache.record_visit(2, "chat");
    cache.record_visit(3, "code");

    // Active is workspace 2 — should be dominant, rest fill in MRU order
    // excluding active: 3 (most recent non-active), then 1.
    auto result = cache.select_futures(2, 100);
    require(result.has_value(), "must return selection");
    require(result->futures.size() == 3, "must yield 3 futures");

    require(result->futures[0].workspace_id == 2, "dominant must be active (2)");
    require(result->futures[0].is_dominant, "dominant flag on position 0");
    require(result->futures[1].workspace_id == 3, "next future should be MRU non-active (3)");
    require(result->futures[2].workspace_id == 1, "last future should be oldest (1)");
}

void test_revisit_moves_to_front() {
    realmheart::services::WorkspaceProphecyCache cache;
    cache.record_visit(1, "web");
    cache.record_visit(2, "chat");
    cache.record_visit(3, "code");

    // Revisit workspace 1 — should move to front.
    cache.record_visit(1, "web");

    auto result = cache.select_futures(3, 0);
    require(result.has_value(), "must return selection");
    // Active=3, then MRU non-active: 1 (just revisited), then 2.
    require(result->futures[0].workspace_id == 3, "dominant must be active (3)");
    require(result->futures[1].workspace_id == 1, "revisited workspace 1 should be first MRU slot");
    require(result->futures[2].workspace_id == 2, "workspace 2 should follow");
}

void test_eviction_at_max_capacity() {
    realmheart::services::WorkspaceProphecyCache cache;

    // Record 8 workspaces (kMaxCacheSize = 8).
    for (int i = 1; i <= 8; ++i) {
        cache.record_visit(i, "ws" + std::to_string(i));
    }
    require(cache.size() == 8, "cache must hold exactly 8 entries");

    // Record a 9th — oldest (workspace 1) should be evicted.
    cache.record_visit(9, "ws9");
    require(cache.size() == 8, "cache must stay at 8 after eviction");
    require(!cache.contains(1), "workspace 1 should be evicted");

    // Active is now 9, futures come from MRU: 9 (dominant), then 8,7,6,5,4 (capped at 6).
    auto result = cache.select_futures(9, 0);
    require(result.has_value(), "must return selection");
    require(result->futures.size() == 6, "must cap at 6 futures");
    require(result->futures[0].workspace_id == 9, "dominant must be active (9)");
    require(result->futures[1].workspace_id == 8, "first MRU future should be 8");
    require(result->futures[5].workspace_id == 4, "last future should be 4 (6th entry, capped)");
}

void test_clear_resets_cache() {
    realmheart::services::WorkspaceProphecyCache cache;
    cache.record_visit(1, "web");
    cache.record_visit(2, "chat");
    require(cache.size() == 2, "should have 2 entries before clear");

    cache.clear();
    require(cache.size() == 0, "must be empty after clear");

    auto result = cache.select_futures(1, 0);
    require(!result.has_value(), "must return nullopt after clear");
}

void test_active_workspace_not_in_cache_still_dominant() {
    realmheart::services::WorkspaceProphecyCache cache;
    cache.record_visit(1, "web");
    cache.record_visit(2, "chat");

    // Active workspace 7 is not in the cache — should still be dominant.
    auto result = cache.select_futures(7, 99);
    require(result.has_value(), "must return selection");
    require(result->futures.size() == 3, "should have dominant + 2 from cache");
    require(result->futures[0].workspace_id == 7, "dominant must be active (7)");
    require(result->futures[0].workspace_name == "7", "uncached active name should be stringified id");
    require(result->futures[0].is_dominant, "position 0 must be dominant");
    require(result->futures[1].workspace_id == 2, "next future should be MRU (2)");
    require(result->futures[2].workspace_id == 1, "last future should be oldest (1)");
}

void test_min_futures_below_threshold() {
    realmheart::services::WorkspaceProphecyCache cache;
    cache.record_visit(1, "web");
    cache.record_visit(2, "chat");

    // Only 2 entries → 2 futures (below kMinFutures=4).
    // The layout engine fills missing slots with wallpaper-only placeholders.
    auto result = cache.select_futures(2, 0);
    require(result.has_value(), "must return selection");
    require(result->futures.size() == 2, "should only have 2 futures from 2 entries");
}

void test_does_not_return_duplicate_workspaces() {
    realmheart::services::WorkspaceProphecyCache cache;
    cache.record_visit(1, "web");
    cache.record_visit(2, "chat");
    cache.record_visit(3, "code");

    auto result = cache.select_futures(3, 0);
    require(result.has_value(), "must return selection");

    bool seen[10] = {};
    for (const auto& f : result->futures) {
        require(!seen[f.workspace_id], "must not return duplicate workspace ids");
        seen[f.workspace_id] = true;
    }
}

void test_contains_lookup() {
    realmheart::services::WorkspaceProphecyCache cache;
    cache.record_visit(5, "five");
    cache.record_visit(10, "ten");

    require(cache.contains(5), "must contain workspace 5");
    require(cache.contains(10), "must contain workspace 10");
    require(!cache.contains(1), "must not contain workspace 1");
    require(!cache.contains(99), "must not contain workspace 99");
}

} // namespace

int main() {
    test_empty_cache_returns_nullopt();
    test_single_visit_dominant_future();
    test_mru_ordering_active_at_front();
    test_revisit_moves_to_front();
    test_eviction_at_max_capacity();
    test_clear_resets_cache();
    test_active_workspace_not_in_cache_still_dominant();
    test_min_futures_below_threshold();
    test_does_not_return_duplicate_workspaces();
    test_contains_lookup();

    std::cout << "All ProphecyCache tests PASSED\n";
    return 0;
}
