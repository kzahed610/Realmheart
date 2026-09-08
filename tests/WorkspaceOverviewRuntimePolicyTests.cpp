#include "ui/workspace/WorkspaceOverviewRuntimePolicy.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_asset_failures_backoff_and_latch() {
    using realmheart::ui::workspace::WorkspaceOverviewAssetRetryPolicy;

    WorkspaceOverviewAssetRetryPolicy policy;
    require(policy.can_attempt(1'000), "a fresh asset policy must allow its first attempt");

    policy.record_failure(1'000);
    require(!policy.can_attempt(1'001), "a failed asset load must be backed off");
    require(policy.can_attempt(51'000), "the first asset retry must open at the backoff deadline");

    policy.record_failure(51'000);
    policy.record_failure(151'000);
    policy.record_failure(351'000);
    policy.record_failure(751'000);
    require(policy.exhausted, "asset failures must eventually latch instead of retrying forever");
    require(!policy.can_attempt(100'000'000), "an exhausted asset policy must stop retrying");

    policy.reset();
    require(policy.can_attempt(0) && !policy.exhausted,
            "monitor or asset invalidation must reopen a latched policy");
}

void test_icon_cache_eviction_includes_failure_entries() {
    using namespace realmheart::ui::workspace;
    require(!workspace_overview_icon_cache_needs_eviction(
                kWorkspaceOverviewIconSurfaceCacheLimit - 1U
            ),
            "the icon cache may fill its final bounded slot");
    require(workspace_overview_icon_cache_needs_eviction(
                kWorkspaceOverviewIconSurfaceCacheLimit
            ),
            "the icon cache must evict before inserting beyond its bound");
}

void test_closing_updates_are_deferred() {
    using namespace realmheart::effects;
    using realmheart::ui::workspace::workspace_overview_defers_snapshot_update;
    require(workspace_overview_defers_snapshot_update(TransitionState::Closing),
            "workspace updates during Closing must await an atomic reversal boundary");
    require(!workspace_overview_defers_snapshot_update(TransitionState::Visible),
            "interactive visible updates must remain immediate");
}

} // namespace

int main() {
    try {
        test_asset_failures_backoff_and_latch();
        test_icon_cache_eviction_includes_failure_entries();
        test_closing_updates_are_deferred();
    } catch (const std::exception& error) {
        std::cerr << "WorkspaceOverviewRuntimePolicyTests failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "Workspace overview runtime policy tests passed\n";
    return 0;
}
