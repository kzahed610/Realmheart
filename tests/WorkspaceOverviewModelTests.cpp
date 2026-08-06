#include "ui/workspace/WorkspaceOverviewModel.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_maps_real_clients_into_realms() {
    realmheart::services::WorkspaceSnapshot snapshot;
    snapshot.available = true;
    snapshot.active_id = 2;
    snapshot.workspaces = {
        {1, "1", 1, false, {{"0x1", "kitty", "build-hybrid"}}},
        {2, "2", 2, true, {
            {"0x2", "org.mozilla.firefox", "Realmheart · GitHub"},
            {"0x3", "org.gnome.Nautilus", "workspace assets"},
        }},
    };

    const auto state =
        realmheart::ui::workspace::build_workspace_overview_state(snapshot);
    require(state[1].active, "workspace II must reflect the active workspace");
    require(state[0].cards[0].app_name == "Kitty",
            "simple classes must be humanized");
    require(state[1].cards[0].app_name == "Firefox",
            "reverse-DNS classes must show their final component");
    require(state[1].cards[0].title == "Realmheart · GitHub",
            "real client titles must be retained");
    require(state[2].card_count == 0,
            "missing workspaces must render as an unobstructed empty realm");
}

void test_limits_cards_and_reports_overflow() {
    realmheart::services::WorkspaceSnapshot snapshot;
    snapshot.available = true;
    snapshot.active_id = 1;
    snapshot.workspaces = {{
        1,
        "1",
        5,
        true,
        {
            {"a", "kitty", "one"},
            {"b", "zen-browser", "two"},
            {"c", "code", "three"},
            {"d", "spotify", "four"},
            {"e", "dolphin", "five"},
        },
    }};

    const auto state =
        realmheart::ui::workspace::build_workspace_overview_state(snapshot);
    require(state[0].card_count == 3,
            "expanded realms must cap visible cards at three");
    require(state[0].cards[2].summary,
            "the final card must become an overflow summary");
    require(state[0].cards[2].app_name == "+3 more",
            "overflow summary must report the hidden client count");
}

void test_card_comparison_ignores_active_state() {
    realmheart::services::WorkspaceSnapshot first;
    first.available = true;
    first.active_id = 1;
    first.workspaces = {{1, "1", 1, true, {{"a", "kitty", "same"}}}};

    auto second = first;
    second.active_id = 2;
    second.workspaces[0].active = false;

    const auto left =
        realmheart::ui::workspace::build_workspace_overview_state(first);
    const auto right =
        realmheart::ui::workspace::build_workspace_overview_state(second);
    require(
        realmheart::ui::workspace::same_workspace_overview_cards(
            left[0], right[0]
        ),
        "active workspace changes must not invalidate cached card textures"
    );
}

} // namespace

int main() {
    try {
        test_maps_real_clients_into_realms();
        test_limits_cards_and_reports_overflow();
        test_card_comparison_ignores_active_state();
    } catch (const std::exception& error) {
        std::cerr << "WorkspaceOverviewModelTests failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "Workspace overview model tests passed\n";
    return 0;
}
