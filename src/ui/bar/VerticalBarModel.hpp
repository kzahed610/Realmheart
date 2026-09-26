#pragma once

#include "services/HyprlandWorkspaces.hpp"

#include <vector>

namespace realmheart::ui::bar {

int workspace_scroll_direction(double vertical_delta) noexcept;

std::vector<realmheart::services::WorkspaceState> build_workspace_pills(
    const realmheart::services::WorkspaceSnapshot& snapshot
);

} // namespace realmheart::ui::bar
