#include "ui/workspace/WorkspaceOverviewModel.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace realmheart::ui::workspace {
namespace {

std::string display_app_name(std::string_view app_id) {
    if (app_id.empty()) return "Unknown application";

    const auto separator = app_id.find_last_of('.');
    if (separator != std::string_view::npos && separator + 1 < app_id.size()) {
        app_id.remove_prefix(separator + 1);
    }

    std::string result(app_id);
    bool capitalize = true;
    for (char& character : result) {
        if (character == '-' || character == '_') {
            character = ' ';
            capitalize = true;
            continue;
        }
        const auto value = static_cast<unsigned char>(character);
        if (capitalize && std::isalpha(value) != 0) {
            character = static_cast<char>(std::toupper(value));
            capitalize = false;
        } else if (!std::isspace(value)) {
            capitalize = false;
        }
    }
    return result.empty() ? "Unknown application" : result;
}

WorkspaceOverviewCard make_window_card(const services::WorkspaceWindow& window) {
    return {
        window.address,
        display_app_name(window.app_id),
        window.title.empty() ? "Untitled window" : window.title,
        false,
    };
}

} // namespace

int workspace_id_for_realm_index(std::size_t realm_index) noexcept {
    return realm_index < kWorkspaceOverviewRealmCount
        ? static_cast<int>(realm_index) + 1
        : 0;
}

int realm_index_for_workspace_id(int workspace_id) noexcept {
    return workspace_id >= 1 &&
        workspace_id <= static_cast<int>(kWorkspaceOverviewRealmCount)
        ? workspace_id - 1
        : -1;
}

WorkspaceOverviewState build_workspace_overview_state(
    const services::WorkspaceSnapshot& snapshot
) {
    WorkspaceOverviewState result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        auto& realm = result[index];
        realm.workspace_id = workspace_id_for_realm_index(index);
        realm.active = snapshot.available && snapshot.active_id == realm.workspace_id;

        const auto workspace = std::find_if(
            snapshot.workspaces.begin(),
            snapshot.workspaces.end(),
            [&realm](const services::WorkspaceState& candidate) {
                return candidate.id == realm.workspace_id;
            }
        );

        if (workspace == snapshot.workspaces.end() ||
            workspace->window_details.empty()) {
            // Empty realms intentionally render no card at all. The landscape,
            // identity, and character are the empty-workspace state.
            realm.card_count = 0;
            realm.total_windows = workspace == snapshot.workspaces.end()
                ? 0
                : workspace->windows;
            continue;
        }

        realm.total_windows = std::max(
            workspace->windows,
            static_cast<int>(workspace->window_details.size())
        );

        if (workspace->window_details.size() <= kWorkspaceOverviewCardLimit) {
            realm.card_count = workspace->window_details.size();
            for (std::size_t card = 0; card < realm.card_count; ++card) {
                realm.cards[card] = make_window_card(workspace->window_details[card]);
            }
            continue;
        }

        realm.card_count = kWorkspaceOverviewCardLimit;
        realm.cards[0] = make_window_card(workspace->window_details[0]);
        realm.cards[1] = make_window_card(workspace->window_details[1]);
        const auto hidden = workspace->window_details.size() - 2U;
        realm.cards[2] = {
            {},
            "+" + std::to_string(hidden) + " more",
            "Additional windows on workspace " +
                std::to_string(realm.workspace_id),
            true,
        };
    }
    return result;
}

bool same_workspace_overview_cards(
    const WorkspaceOverviewRealm& left,
    const WorkspaceOverviewRealm& right
) noexcept {
    if (left.workspace_id != right.workspace_id ||
        left.total_windows != right.total_windows ||
        left.card_count != right.card_count) {
        return false;
    }
    for (std::size_t index = 0; index < left.card_count; ++index) {
        if (left.cards[index] != right.cards[index]) return false;
    }
    return true;
}

} // namespace realmheart::ui::workspace
