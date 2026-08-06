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
    require(state[1].cards[0].icon_name == "org.mozilla.firefox",
            "raw application identifiers must be retained for icon lookup");
    require(state[1].cards[0].title == "Realmheart · GitHub",
            "real client titles must be retained");
    require(state[1].cards[0].address == "0x2",
            "real client addresses must be retained for exact focusing");
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

void test_viewport_tracks_workspaces_beyond_four() {
    realmheart::services::WorkspaceSnapshot snapshot;
    snapshot.available = true;
    snapshot.active_id = 5;
    snapshot.workspaces = {
        {2, "2", 0, false, {}},
        {3, "3", 0, false, {}},
        {4, "4", 0, false, {}},
        {5, "5", 1, true, {{"0x5", "kitty", "workspace five"}}},
    };

    const int first =
        realmheart::ui::workspace::visible_workspace_start_for_active(
            snapshot.active_id
        );
    const auto state =
        realmheart::ui::workspace::build_workspace_overview_state(
            snapshot,
            first
        );

    require(first == 2,
            "workspace five must shift the four-slot viewport to start at two");
    require(state[0].workspace_id == 2 && state[3].workspace_id == 5,
            "the viewport must expose workspaces two through five");
    require(state[3].active,
            "workspace five must be active in the final visible slot");
    require(state[3].cards[0].address == "0x5",
            "workspace five clients must populate the cycled realm");
}

void test_elemental_styles_and_numerals_cycle() {
    using realmheart::ui::workspace::style_index_for_workspace_id;
    using realmheart::ui::workspace::workspace_roman_numeral;

    require(style_index_for_workspace_id(1) == 0,
            "workspace one must use the Fire style");
    require(style_index_for_workspace_id(4) == 3,
            "workspace four must use the Earth style");
    require(style_index_for_workspace_id(5) == 0,
            "workspace five must cycle back to Fire and Bairon");
    require(style_index_for_workspace_id(6) == 1,
            "workspace six must cycle to Water and Varay");
    require(workspace_roman_numeral(5) == "V",
            "workspace five must display Roman numeral V");
    require(workspace_roman_numeral(14) == "XIV",
            "Roman numerals must support later workspaces");
}

} // namespace

int main() {
    try {
        test_maps_real_clients_into_realms();
        test_limits_cards_and_reports_overflow();
        test_card_comparison_ignores_active_state();
        test_viewport_tracks_workspaces_beyond_four();
        test_elemental_styles_and_numerals_cycle();
    } catch (const std::exception& error) {
        std::cerr << "WorkspaceOverviewModelTests failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "Workspace overview model tests passed\n";
    return 0;
}
