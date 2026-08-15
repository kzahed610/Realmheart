#include "ui/bar/VerticalBarModel.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_first_four_workspaces_remain_stable() {
    realmheart::services::WorkspaceSnapshot snapshot;
    snapshot.available = true;
    snapshot.active_id = 2;
    snapshot.workspaces = {
        {1, "1", 1, false, {}},
        {2, "2", 2, true, {}},
        {4, "4", 1, false, {}},
    };

    const auto pills = realmheart::ui::bar::build_workspace_pills(snapshot);
    require(pills.size() == 4, "the taskbar must expose four workspace runes");
    require(pills[0].id == 1 && pills[3].id == 4,
            "low workspace IDs must keep the 1-4 viewport");
    require(pills[1].active, "workspace two must remain active");
    require(pills[1].windows == 2,
            "workspace state must survive viewport projection");
}

void test_high_workspace_ids_slide_as_a_four_slot_viewport() {
    realmheart::services::WorkspaceSnapshot snapshot;
    snapshot.available = true;
    snapshot.active_id = 9;
    snapshot.workspaces = {
        {6, "6", 1, false, {}},
        {7, "7", 0, false, {}},
        {8, "8", 3, false, {}},
        {9, "9", 2, true, {}},
    };

    const auto pills = realmheart::ui::bar::build_workspace_pills(snapshot);
    require(pills.size() == 4, "the viewport must keep exactly four runes");
    require(pills[0].id == 6 && pills[1].id == 7 &&
                pills[2].id == 8 && pills[3].id == 9,
            "workspace nine must project the 6-9 viewport");
    require(pills[3].active, "the active high workspace must stay visible");
    require(pills[2].windows == 3,
            "occupied state must map by actual workspace ID");
}

void test_unavailable_snapshot_falls_back_to_workspace_one() {
    realmheart::services::WorkspaceSnapshot snapshot;
    snapshot.available = false;
    snapshot.active_id = 99;

    const auto pills = realmheart::ui::bar::build_workspace_pills(snapshot);
    require(pills.size() == 4, "fallback must still expose four runes");
    require(pills[0].id == 1 && pills[3].id == 4,
            "unavailable compositor data must fall back to 1-4");
}

} // namespace

int main() {
    try {
        test_first_four_workspaces_remain_stable();
        test_high_workspace_ids_slide_as_a_four_slot_viewport();
        test_unavailable_snapshot_falls_back_to_workspace_one();
    } catch (const std::exception& error) {
        std::cerr << "WorkspaceMorphViewportTests failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Workspace morph viewport tests passed\n";
    return 0;
}
