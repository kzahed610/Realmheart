#include "services/HyprlandWorkspaces.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}


void test_switch_to_dispatches_requested_workspace() {
    const auto root = std::filesystem::temp_directory_path() /
        ("realmheart-workspace-switch-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto executable = root / "hyprctl";
    const auto output = root / "arguments.txt";
    {
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "printf '%s\\n' \"$*\" > \"$REALMHEART_HYPRCTL_TEST_OUTPUT\"\n";
    }
    std::filesystem::permissions(
        executable,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace
    );

    const char* old_path_value = std::getenv("PATH");
    const std::string old_path = old_path_value == nullptr ? std::string{} : old_path_value;
    const std::string test_path = root.string() + (old_path.empty() ? "" : ":" + old_path);
    ::setenv("PATH", test_path.c_str(), 1);
    ::setenv("REALMHEART_HYPRCTL_TEST_OUTPUT", output.string().c_str(), 1);

    const bool switched = realmheart::services::HyprlandWorkspaces::switch_to(4);

    ::setenv("PATH", old_path.c_str(), 1);
    ::unsetenv("REALMHEART_HYPRCTL_TEST_OUTPUT");

    require(switched, "workspace dispatch command must report success");
    std::ifstream recorded(output);
    std::string arguments;
    std::getline(recorded, arguments);
    require(arguments == "dispatch hl.dsp.focus({ workspace = 4 })",
            "workspace dispatch must use Hyprland Lua focus syntax with the exact requested workspace id");
    require(!realmheart::services::HyprlandWorkspaces::switch_to(0),
            "invalid workspace ids must be rejected before spawning hyprctl");

    std::filesystem::remove_all(root);
}

void test_switch_to_rejects_lua_dispatch_errors() {
    const auto root = std::filesystem::temp_directory_path() /
        ("realmheart-workspace-switch-error-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto executable = root / "hyprctl";
    {
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "printf '%s\\n' 'error: simulated Lua dispatch parser failure'\n"
               << "exit 0\n";
    }
    std::filesystem::permissions(
        executable,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace
    );

    const char* old_path_value = std::getenv("PATH");
    const std::string old_path = old_path_value == nullptr ? std::string{} : old_path_value;
    const std::string test_path = root.string() + (old_path.empty() ? "" : ":" + old_path);
    ::setenv("PATH", test_path.c_str(), 1);

    const bool switched = realmheart::services::HyprlandWorkspaces::switch_to(4);

    ::setenv("PATH", old_path.c_str(), 1);
    require(!switched,
            "Hyprland Lua dispatch parser errors must not be treated as successful workspace switches");
    std::filesystem::remove_all(root);
}

void test_fixture_parses_sorted_deduplicated_state() {
    constexpr auto active = R"({"id":3,"name":"dev"})";
    constexpr auto workspaces = R"([
        {"id":5,"name":"five","windows":1},
        {"id":3,"name":"dev","windows":2},
        {"id":2,"name":"two","windows":0},
        {"id":2,"name":"duplicate","windows":99},
        {"id":-1,"name":"special","windows":7}
    ])";

    const auto snapshot = realmheart::services::HyprlandWorkspaces::parse(active, workspaces);

    require(snapshot.available, "valid fixture must produce an available snapshot");
    require(snapshot.active_id == 3, "active workspace id must come from the active fixture");
    require(snapshot.workspaces.size() == 3, "invalid and duplicate workspace ids must be omitted");
    require(snapshot.workspaces[0].id == 2 && snapshot.workspaces[1].id == 3 && snapshot.workspaces[2].id == 5,
            "workspace state must be sorted by id");
    require(snapshot.workspaces[1].active, "active workspace must be marked active");
    require(!snapshot.workspaces[0].active && !snapshot.workspaces[2].active,
            "only the active workspace may be marked active");
    require(snapshot.workspaces[1].windows == 2 && snapshot.workspaces[1].name == "dev",
            "workspace metadata must be retained");
}

void test_active_workspace_is_synthesized_when_list_omits_it() {
    const auto snapshot = realmheart::services::HyprlandWorkspaces::parse(
        R"({"id":4})",
        R"([{"id":2,"name":"two","windows":1}])"
    );

    require(snapshot.available, "valid state with an omitted active workspace must remain available");
    require(snapshot.workspaces.size() == 2, "missing active workspace must be synthesized");
    require(snapshot.workspaces[1].id == 4 && snapshot.workspaces[1].active,
            "synthesized active workspace must be sorted and marked active");
}

void test_malformed_active_fixture_is_unavailable() {
    const auto snapshot = realmheart::services::HyprlandWorkspaces::parse(
        R"({"name":"missing-id"})",
        "[]"
    );

    require(!snapshot.available, "missing active id must produce unavailable state");
    require(!snapshot.error.empty(), "parser failure must carry a deterministic reason");
}

void test_malformed_workspace_fixture_is_unavailable() {
    const auto snapshot = realmheart::services::HyprlandWorkspaces::parse(
        R"({"id":1})",
        "not-json"
    );

    require(!snapshot.available, "malformed workspace input must not masquerade as live state");
}

void test_clients_are_attached_to_their_workspaces() {
    constexpr auto clients = R"([
        {
            "address":"0xaaa",
            "class":"org.mozilla.firefox",
            "title":"Realmheart – Mozilla Firefox",
            "workspace":{"id":1}
        },
        {
            "address":"0xbbb",
            "initialClass":"org.kde.dolphin",
            "initialTitle":"Home — Dolphin",
            "workspace":{"id":3}
        },
        {"address":"0xignored","class":"special","workspace":{"id":-99}}
    ])";

    const auto snapshot = realmheart::services::HyprlandWorkspaces::parse(
        R"({"id":1})",
        R"([{"id":1,"name":"one","windows":0},{"id":3,"name":"three","windows":0}])",
        clients
    );

    require(snapshot.available, "valid client metadata must keep workspace state available");
    require(snapshot.workspaces[0].window_details.size() == 1,
            "workspace one must receive its browser window");
    require(snapshot.workspaces[0].window_details[0].app_id == "org.mozilla.firefox",
            "window application id must come from the current class");
    require(snapshot.workspaces[0].window_details[0].title == "Realmheart – Mozilla Firefox",
            "window title must be preserved");
    require(snapshot.workspaces[0].windows == 1,
            "client metadata must prevent an occupied workspace from appearing empty");
    require(snapshot.workspaces[1].window_details[0].app_id == "org.kde.dolphin",
            "initial class must be used when the current class is absent");
}


void test_monitor_specific_fixture_selects_each_output_workspace() {
    constexpr auto monitors = R"([
        {"name":"HDMI-A-1","activeWorkspace":{"id":2,"name":"2"}},
        {"name":"DP-1","activeWorkspace":{"id":7,"name":"7"}}
    ])";
    constexpr auto workspaces = R"([
        {"id":2,"name":"two","windows":1},
        {"id":7,"name":"seven","windows":2}
    ])";

    const auto hdmi = realmheart::services::HyprlandWorkspaces::parse_for_monitor(
        "HDMI-A-1", monitors, workspaces
    );
    const auto dp = realmheart::services::HyprlandWorkspaces::parse_for_monitor(
        "DP-1", monitors, workspaces
    );

    require(hdmi.available && hdmi.active_id == 2,
            "HDMI bar must use HDMI-A-1's active workspace");
    require(dp.available && dp.active_id == 7,
            "DP bar must use DP-1's active workspace");
    require(!realmheart::services::HyprlandWorkspaces::parse_for_monitor(
                "missing", monitors, workspaces
            ).available,
            "missing output must fail deterministically instead of borrowing another monitor's workspace");
}

void test_switch_to_on_monitor_focuses_output_before_workspace() {
    const auto root = std::filesystem::temp_directory_path() /
        ("realmheart-workspace-monitor-switch-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto executable = root / "hyprctl";
    const auto output = root / "arguments.txt";
    {
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "printf '%s\\n' \"$*\" >> \"$REALMHEART_HYPRCTL_TEST_OUTPUT\"\n";
    }
    std::filesystem::permissions(
        executable,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace
    );

    const char* old_path_value = std::getenv("PATH");
    const std::string old_path = old_path_value == nullptr ? std::string{} : old_path_value;
    const std::string test_path = root.string() + (old_path.empty() ? "" : ":" + old_path);
    ::setenv("PATH", test_path.c_str(), 1);
    ::setenv("REALMHEART_HYPRCTL_TEST_OUTPUT", output.string().c_str(), 1);

    const bool switched = realmheart::services::HyprlandWorkspaces::switch_to_on_monitor(
        6, "DP-1"
    );

    ::setenv("PATH", old_path.c_str(), 1);
    ::unsetenv("REALMHEART_HYPRCTL_TEST_OUTPUT");

    require(switched, "monitor-specific workspace dispatch must report success");
    std::ifstream recorded(output);
    std::string first;
    std::string second;
    std::getline(recorded, first);
    std::getline(recorded, second);
    require(first == "dispatch hl.dsp.focus({ monitor = \"DP-1\" })",
            "monitor-specific switch must focus the exact connector first");
    require(second == "dispatch hl.dsp.focus({ workspace = 6, on_current_monitor = true })",
            "monitor-specific switch must then select the requested workspace");

    std::filesystem::remove_all(root);
}


void test_switch_to_named_on_monitor_focuses_output_before_named_workspace() {
    const auto root = std::filesystem::temp_directory_path() /
        ("realmheart-named-workspace-monitor-switch-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto executable = root / "hyprctl";
    const auto output = root / "arguments.txt";
    {
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "printf '%s\\n' \"$*\" >> \"$REALMHEART_HYPRCTL_TEST_OUTPUT\"\n";
    }
    std::filesystem::permissions(
        executable,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace
    );

    const char* old_path_value = std::getenv("PATH");
    const std::string old_path = old_path_value == nullptr ? std::string{} : old_path_value;
    const std::string test_path = root.string() + (old_path.empty() ? "" : ":" + old_path);
    ::setenv("PATH", test_path.c_str(), 1);
    ::setenv("REALMHEART_HYPRCTL_TEST_OUTPUT", output.string().c_str(), 1);

    const bool switched = realmheart::services::HyprlandWorkspaces::switch_to_named_on_monitor(
        "realmheart-mana-cores", "HDMI-A-1"
    );

    ::setenv("PATH", old_path.c_str(), 1);
    ::unsetenv("REALMHEART_HYPRCTL_TEST_OUTPUT");

    require(switched, "monitor-specific named workspace dispatch must report success");
    std::ifstream recorded(output);
    std::string first;
    std::string second;
    std::getline(recorded, first);
    std::getline(recorded, second);
    require(first == "dispatch hl.dsp.focus({ monitor = \"HDMI-A-1\" })",
            "named workspace switch must focus the exact connector first");
    require(second == "dispatch hl.dsp.focus({ workspace = \"name:realmheart-mana-cores\", on_current_monitor = true })",
            "named workspace switch must then select the named workspace");

    std::filesystem::remove_all(root);
}

void test_malformed_clients_fixture_is_unavailable() {
    const auto snapshot = realmheart::services::HyprlandWorkspaces::parse(
        R"({"id":1})",
        R"([{"id":1,"windows":0}])",
        "not-json"
    );
    require(!snapshot.available, "malformed client data must fail deterministically");
}

} // namespace

int main() {
    test_switch_to_dispatches_requested_workspace();
    test_switch_to_rejects_lua_dispatch_errors();
    test_fixture_parses_sorted_deduplicated_state();
    test_active_workspace_is_synthesized_when_list_omits_it();
    test_malformed_active_fixture_is_unavailable();
    test_malformed_workspace_fixture_is_unavailable();
    test_clients_are_attached_to_their_workspaces();
    test_monitor_specific_fixture_selects_each_output_workspace();
    test_switch_to_on_monitor_focuses_output_before_workspace();
    test_switch_to_named_on_monitor_focuses_output_before_named_workspace();
    test_malformed_clients_fixture_is_unavailable();
    std::cout << "Workspace parser tests passed\n";
    return 0;
}
