#include "ui/bar/VerticalBarModel.hpp"

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

void test_workspace_pills_show_four_and_merge_live_state() {
    realmheart::services::WorkspaceSnapshot snapshot;
    snapshot.available = true;
    snapshot.active_id = 3;
    snapshot.workspaces = {
        {3, "dev", 2, true, {}},
        {5, "five", 1, false, {}},
        {7, "seven", 1, false, {}},
    };

    const auto pills = realmheart::ui::bar::build_workspace_pills(snapshot);
    require(pills.size() == 4, "bar must show exactly four workspace runes");
    require(pills.front().id == 1 && pills.back().id == 4,
            "workspaces 1-4 must be visible while the active workspace is below 5");
    require(pills[2].id == 3 && pills[2].active && pills[2].windows == 2,
            "live workspace state must replace its default rune");
}

void test_workspace_window_slides_only_for_workspace_five() {
    realmheart::services::WorkspaceSnapshot snapshot;
    snapshot.available = true;
    snapshot.active_id = 5;
    snapshot.workspaces = {{5, "five", 1, true, {}}};

    const auto pills = realmheart::ui::bar::build_workspace_pills(snapshot);
    require(pills.size() == 4, "sliding range must still contain four runes");
    require(pills.front().id == 2 && pills.back().id == 5,
            "workspace five must slide the visible range to 2-5");
    require(pills.back().active, "workspace five must remain the bottom active rune");

    snapshot.active_id = 3;
    const auto reset = realmheart::ui::bar::build_workspace_pills(snapshot);
    require(reset.front().id == 1 && reset.back().id == 4,
            "returning below workspace five must restore 1-4");
}

void test_unavailable_state_still_has_stable_targets() {
    const realmheart::services::WorkspaceSnapshot snapshot;
    const auto pills = realmheart::ui::bar::build_workspace_pills(snapshot);
    require(pills.size() == 4, "unavailable Hyprland state must retain four click targets");
    require(pills.front().id == 1 && pills.back().id == 4,
            "fallback click targets must remain workspaces 1-4");
    for (const auto& pill : pills) {
        require(!pill.active && pill.windows == 0,
                "unavailable workspace state must not invent activity");
    }
}

} // namespace

int main() {
    test_workspace_pills_show_four_and_merge_live_state();
    test_workspace_window_slides_only_for_workspace_five();
    test_unavailable_state_still_has_stable_targets();
    std::cout << "Vertical bar model tests passed\n";
    return 0;
}
