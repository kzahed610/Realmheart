#include "services/HyprlandWorkspaces.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_fixture_parses_sorted_deduplicated_state() {
    constexpr auto active = R"({"id":3,"name":"dev"})";
    constexpr auto workspaces = R"([
        {"id":5,"name":"five","windows":1},
        {"id":3,"name":"dev","windows":2},
        {"id":2,"name":"two","windows":0},
        {"id":2,"name":"duplicate","windows":99},
        {"id":-1,"name":"special","windows":7}
    ])";

    const auto snapshot = realmheart::services::HyprlandWorkspaces::parse(active, workspaces);

    require(snapshot.available, "valid fixture must produce an available snapshot");
    require(snapshot.active_id == 3, "active workspace id must come from the active fixture");
    require(snapshot.workspaces.size() == 3, "invalid and duplicate workspace ids must be omitted");
    require(snapshot.workspaces[0].id == 2 && snapshot.workspaces[1].id == 3 && snapshot.workspaces[2].id == 5,
            "workspace state must be sorted by id");
    require(snapshot.workspaces[1].active, "active workspace must be marked active");
    require(!snapshot.workspaces[0].active && !snapshot.workspaces[2].active,
            "only the active workspace may be marked active");
    require(snapshot.workspaces[1].windows == 2 && snapshot.workspaces[1].name == "dev",
            "workspace metadata must be retained");
}

void test_active_workspace_is_synthesized_when_list_omits_it() {
    const auto snapshot = realmheart::services::HyprlandWorkspaces::parse(
        R"({"id":4})",
        R"([{"id":2,"name":"two","windows":1}])"
    );

    require(snapshot.available, "valid state with an omitted active workspace must remain available");
    require(snapshot.workspaces.size() == 2, "missing active workspace must be synthesized");
    require(snapshot.workspaces[1].id == 4 && snapshot.workspaces[1].active,
            "synthesized active workspace must be sorted and marked active");
}

void test_malformed_active_fixture_is_unavailable() {
    const auto snapshot = realmheart::services::HyprlandWorkspaces::parse(
        R"({"name":"missing-id"})",
        "[]"
    );

    require(!snapshot.available, "missing active id must produce unavailable state");
    require(!snapshot.error.empty(), "parser failure must carry a deterministic reason");
}

void test_malformed_workspace_fixture_is_unavailable() {
    const auto snapshot = realmheart::services::HyprlandWorkspaces::parse(
        R"({"id":1})",
        "not-json"
    );

    require(!snapshot.available, "malformed workspace input must not masquerade as live state");
}

} // namespace

int main() {
    test_fixture_parses_sorted_deduplicated_state();
    test_active_workspace_is_synthesized_when_list_omits_it();
    test_malformed_active_fixture_is_unavailable();
    test_malformed_workspace_fixture_is_unavailable();
    std::cout << "Workspace parser tests passed\n";
    return 0;
}
