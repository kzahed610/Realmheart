#include "ui/bar/WorkspaceWindowTracker.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

realmheart::services::WorkspaceWindow window(
    std::string address,
    std::string app,
    std::string title
) {
    return {std::move(address), std::move(app), std::move(title)};
}

void test_first_seen_order_survives_hyprland_reordering() {
    realmheart::ui::bar::WorkspaceWindowTracker tracker;

    realmheart::services::WorkspaceSnapshot first;
    first.workspaces = {{1, "one", 2, true, {
        window("0xa", "firefox", "First"),
        window("0xb", "dolphin", "Second"),
    }}};
    tracker.apply(first);

    realmheart::services::WorkspaceSnapshot second;
    second.workspaces = {{1, "one", 3, true, {
        window("0xc", "code", "Third"),
        window("0xb", "dolphin", "Second — renamed"),
        window("0xa", "firefox", "First — renamed"),
    }}};
    tracker.apply(second);

    const auto& windows = second.workspaces.front().window_details;
    require(windows.size() == 3, "all live windows must remain in the preview model");
    require(windows[0].address == "0xa" && windows[1].address == "0xb" && windows[2].address == "0xc",
            "window preview order must follow first appearance, not snapshot order");
    require(windows[0].title == "First — renamed",
            "title changes must not reset a window's original sequence");
}

void test_closed_windows_release_their_sequence() {
    realmheart::ui::bar::WorkspaceWindowTracker tracker;

    realmheart::services::WorkspaceSnapshot first;
    first.workspaces = {{1, "one", 1, true, {window("0xa", "firefox", "Old")}}};
    tracker.apply(first);

    realmheart::services::WorkspaceSnapshot empty;
    empty.workspaces = {{1, "one", 0, true, {}}};
    tracker.apply(empty);

    realmheart::services::WorkspaceSnapshot reopened;
    reopened.workspaces = {{1, "one", 2, true, {
        window("0xb", "code", "Existing"),
        window("0xa", "firefox", "Reopened"),
    }}};
    tracker.apply(reopened);

    const auto& windows = reopened.workspaces.front().window_details;
    require(windows[0].address == "0xb" && windows[1].address == "0xa",
            "a closed and reopened address must be treated as newly seen");
}

} // namespace

int main() {
    test_first_seen_order_survives_hyprland_reordering();
    test_closed_windows_release_their_sequence();
    std::cout << "Workspace window tracker tests passed\n";
    return 0;
}
