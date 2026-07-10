#pragma once

#include "services/HyprlandWorkspaces.hpp"
#include "services/Notifications.hpp"
#include "services/RightSidebarServices.hpp"

#include <string>
#include <vector>

namespace realmheart::ui::bar {

struct BarStatusSlot {
    std::string name;
    std::string icon_name;
    std::string fallback_text;
    std::string tooltip;
    std::string badge_text;
    bool enabled = false;
};

std::vector<realmheart::services::WorkspaceState> build_workspace_pills(
    const realmheart::services::WorkspaceSnapshot& snapshot
);

std::vector<BarStatusSlot> build_status_slots(
    const std::vector<realmheart::services::ServiceStatus>& report,
    const realmheart::services::NotificationSnapshot& notifications
);

} // namespace realmheart::ui::bar
