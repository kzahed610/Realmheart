#pragma once

#include "services/HyprlandWorkspaces.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace realmheart::ui::workspace {

inline constexpr std::size_t kWorkspaceOverviewRealmCount = 4;
inline constexpr std::size_t kWorkspaceOverviewCardLimit = 3;

struct WorkspaceOverviewCard {
    std::string address;
    std::string icon_name;
    std::string app_name;
    std::string title;
    bool summary = false;

    bool operator==(const WorkspaceOverviewCard&) const = default;
};

struct WorkspaceOverviewRealm {
    int workspace_id = 1;
    bool active = false;
    int total_windows = 0;
    std::array<WorkspaceOverviewCard, kWorkspaceOverviewCardLimit> cards{};
    std::size_t card_count = 0;
};

using WorkspaceOverviewState =
    std::array<WorkspaceOverviewRealm, kWorkspaceOverviewRealmCount>;

[[nodiscard]] WorkspaceOverviewState build_workspace_overview_state(
    const services::WorkspaceSnapshot& snapshot
);

[[nodiscard]] bool same_workspace_overview_cards(
    const WorkspaceOverviewRealm& left,
    const WorkspaceOverviewRealm& right
) noexcept;

[[nodiscard]] int realm_index_for_workspace_id(int workspace_id) noexcept;
[[nodiscard]] int workspace_id_for_realm_index(std::size_t realm_index) noexcept;

} // namespace realmheart::ui::workspace
