#pragma once

#include "services/HyprlandWorkspaces.hpp"

#include <vector>

namespace realmheart::ui::bar {

std::vector<realmheart::services::WorkspaceState> build_workspace_pills(
    const realmheart::services::WorkspaceSnapshot& snapshot
);

} // namespace realmheart::ui::bar
