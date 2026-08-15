#pragma once

#include "core/Command.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realmheart::services {

struct WorkspaceWindow {
    std::string address;
    std::string app_id;
    std::string title;
};

struct WorkspaceState {
    int id = 0;
    std::string name;
    int windows = 0;
    bool active = false;
    std::vector<WorkspaceWindow> window_details;
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
    static bool switch_to(
        int workspace_id,
        const realmheart::core::CommandOptions& options = {}
    );
    static bool switch_to_named(
        std::string_view workspace_name,
        const realmheart::core::CommandOptions& options = {}
    );
    [[nodiscard]] static std::optional<int> active_workspace_id(
        const realmheart::core::CommandOptions& options = {}
    );
    static WorkspaceSnapshot parse(
        std::string_view active_json,
        std::string_view workspaces_json,
        std::string_view clients_json = "[]"
    );
    static std::string describe(const WorkspaceSnapshot& snapshot);
};

} // namespace realmheart::services
