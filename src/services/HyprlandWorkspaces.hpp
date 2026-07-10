#pragma once

#include "core/Command.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace realmheart::services {

struct WorkspaceState {
    int id = 0;
    std::string name;
    int windows = 0;
    bool active = false;
};

struct WorkspaceSnapshot {
    bool available = false;
    int active_id = 1;
    std::string error;
    std::vector<WorkspaceState> workspaces;
};

class HyprlandWorkspaces {
public:
    static WorkspaceSnapshot read(const realmheart::core::CommandOptions& options = {});
    static WorkspaceSnapshot parse(std::string_view active_json, std::string_view workspaces_json);
    static std::string describe(const WorkspaceSnapshot& snapshot);
};

} // namespace realmheart::services
