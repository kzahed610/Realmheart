#pragma once

#include "services/HyprlandWorkspaces.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace realmheart::ui::bar {

// Preserves first-seen ordering across Hyprland snapshots so the hover preview
// remains stable while titles change or windows move between workspaces.
class WorkspaceWindowTracker {
public:
    void apply(services::WorkspaceSnapshot& snapshot);

private:
    std::unordered_map<std::string, std::uint64_t> sequence_by_address_;
    std::uint64_t next_sequence_ = 1;
};

} // namespace realmheart::ui::bar
