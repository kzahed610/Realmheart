#pragma once

#include "services/HyprlandWorkspaces.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace realmheart::ui::workspace {

inline constexpr std::size_t kWorkspaceOverviewRealmCount = 4;
inline constexpr std::size_t kWorkspaceOverviewPreviewCardLimit = 3;
inline constexpr std::size_t kWorkspaceOverviewCardLimit =
    kWorkspaceOverviewPreviewCardLimit;

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
    std::vector<WorkspaceOverviewCard> windows{};
};

using WorkspaceOverviewState =
    std::array<WorkspaceOverviewRealm, kWorkspaceOverviewRealmCount>;

[[nodiscard]] WorkspaceOverviewState build_workspace_overview_state(
    const services::WorkspaceSnapshot& snapshot,
    int first_workspace_id = 1
);

[[nodiscard]] bool same_workspace_overview_cards(
    const WorkspaceOverviewRealm& left,
    const WorkspaceOverviewRealm& right
) noexcept;

[[nodiscard]] int visible_workspace_start_for_active(int workspace_id) noexcept;
[[nodiscard]] std::size_t style_index_for_workspace_id(int workspace_id) noexcept;
[[nodiscard]] std::string workspace_roman_numeral(int workspace_id);
[[nodiscard]] int realm_index_for_workspace_id(
    int workspace_id,
    int first_workspace_id = 1
) noexcept;
[[nodiscard]] int workspace_id_for_realm_index(
    std::size_t realm_index,
    int first_workspace_id = 1
) noexcept;

} // namespace realmheart::ui::workspace
