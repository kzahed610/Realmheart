#include "ui/workspace/WorkspaceOverviewModel.hpp"

#include <algorithm>
#include <array>
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
        window.app_id.empty() ? "application-x-executable" : window.app_id,
        display_app_name(window.app_id),
        window.title.empty() ? "Untitled window" : window.title,
        false,
    };
}

} // namespace

int visible_workspace_start_for_active(int workspace_id) noexcept {
    const int active = std::max(1, workspace_id);
    return std::max(
        1,
        active - static_cast<int>(kWorkspaceOverviewRealmCount) + 1
    );
}

std::size_t style_index_for_workspace_id(int workspace_id) noexcept {
    const int normalized = std::max(1, workspace_id) - 1;
    return static_cast<std::size_t>(
        normalized % static_cast<int>(kWorkspaceOverviewRealmCount)
    );
}

std::string workspace_roman_numeral(int workspace_id) {
    if (workspace_id <= 0) return std::to_string(workspace_id);
    if (workspace_id > 3999) return std::to_string(workspace_id);

    struct RomanToken {
        int value;
        std::string_view text;
    };
    constexpr std::array<RomanToken, 13> kTokens{{
        {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"},
        {100, "C"}, {90, "XC"}, {50, "L"}, {40, "XL"},
        {10, "X"}, {9, "IX"}, {5, "V"}, {4, "IV"}, {1, "I"},
    }};

    std::string result;
    int remaining = workspace_id;
    for (const auto& token : kTokens) {
        while (remaining >= token.value) {
            result.append(token.text);
            remaining -= token.value;
        }
    }
    return result;
}

int workspace_id_for_realm_index(
    std::size_t realm_index,
    int first_workspace_id
) noexcept {
    return realm_index < kWorkspaceOverviewRealmCount
        ? std::max(1, first_workspace_id) + static_cast<int>(realm_index)
        : 0;
}

int realm_index_for_workspace_id(
    int workspace_id,
    int first_workspace_id
) noexcept {
    const int first = std::max(1, first_workspace_id);
    const int offset = workspace_id - first;
    return offset >= 0 &&
        offset < static_cast<int>(kWorkspaceOverviewRealmCount)
        ? offset
        : -1;
}

WorkspaceOverviewState build_workspace_overview_state(
    const services::WorkspaceSnapshot& snapshot,
    int first_workspace_id
) {
    WorkspaceOverviewState result{};
    const int first = std::max(1, first_workspace_id);
    for (std::size_t index = 0; index < result.size(); ++index) {
        auto& realm = result[index];
        realm.workspace_id = workspace_id_for_realm_index(index, first);
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
        realm.windows.reserve(workspace->window_details.size());
        for (const auto& window : workspace->window_details) {
            realm.windows.push_back(make_window_card(window));
        }

        if (realm.windows.size() <= kWorkspaceOverviewCardLimit) {
            realm.card_count = realm.windows.size();
            for (std::size_t card = 0; card < realm.card_count; ++card) {
                realm.cards[card] = realm.windows[card];
            }
            continue;
        }

        realm.card_count = kWorkspaceOverviewCardLimit;
        realm.cards[0] = realm.windows[0];
        realm.cards[1] = realm.windows[1];
        const auto hidden = realm.windows.size() - 2U;
        realm.cards[2] = {
            {},
            "view-more-symbolic",
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
        left.card_count != right.card_count ||
        left.windows != right.windows) {
        return false;
    }
    for (std::size_t index = 0; index < left.card_count; ++index) {
        if (left.cards[index] != right.cards[index]) return false;
    }
    return true;
}

} // namespace realmheart::ui::workspace
