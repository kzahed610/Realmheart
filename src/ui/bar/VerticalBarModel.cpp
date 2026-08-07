#include "ui/bar/VerticalBarModel.hpp"

#include <algorithm>
#include <string>

namespace realmheart::ui::bar {

std::vector<realmheart::services::WorkspaceState> build_workspace_pills(
    const realmheart::services::WorkspaceSnapshot& snapshot
) {
    constexpr int kVisibleWorkspaceCount = 4;

    const int active_id = std::max(
        1,
        snapshot.available ? snapshot.active_id : 1
    );
    const int first_visible = std::max(
        1,
        active_id - kVisibleWorkspaceCount + 1
    );

    std::vector<realmheart::services::WorkspaceState> workspaces;
    workspaces.reserve(kVisibleWorkspaceCount);
    for (int id = first_visible; id < first_visible + kVisibleWorkspaceCount; ++id) {
        realmheart::services::WorkspaceState state;
        state.id = id;
        state.name = std::to_string(id);
        state.active = snapshot.available && id == active_id;

        const auto live = std::find_if(
            snapshot.workspaces.begin(), snapshot.workspaces.end(),
            [id](const auto& item) { return item.id == id; }
        );
        if (live != snapshot.workspaces.end()) state = *live;
        state.active = snapshot.available && id == active_id;
        workspaces.push_back(std::move(state));
    }
    return workspaces;
}

} // namespace realmheart::ui::bar
