#include "core/ShellCommand.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void parses_supported_shell_commands() {
    using realmheart::core::ShellCommand;
    using realmheart::core::parse_shell_command;

    require(parse_shell_command("sidebar-right-toggle") == ShellCommand::ToggleRightSidebar,
            "sidebar-right-toggle should parse");
    require(parse_shell_command("bar-toggle") == ShellCommand::ToggleBar,
            "bar-toggle should parse");
    require(parse_shell_command("character-toggle") == ShellCommand::ToggleCharacter,
            "character-toggle should parse");
    require(parse_shell_command("character-hair-mode") == ShellCommand::SetCharacterHairMode,
            "character-hair-mode should parse");
    require(parse_shell_command("start-recording") == ShellCommand::StartRecording,
            "start-recording should parse");
    require(parse_shell_command("stop-recording") == ShellCommand::StopRecording,
            "stop-recording should parse");
    require(parse_shell_command("toggle-notes") == ShellCommand::ToggleNotes,
            "toggle-notes should parse");
    require(parse_shell_command("set-wallpaper-path") == ShellCommand::SetWallpaperPath,
            "set-wallpaper-path should parse");
    require(parse_shell_command("set-wallpaper-backend") == ShellCommand::SetWallpaperBackend,
            "set-wallpaper-backend should parse");
    require(parse_shell_command("launch-launcher-query") == ShellCommand::LaunchLauncherQuery,
            "launch-launcher-query should parse");
    require(parse_shell_command("workspace-overview-toggle") == ShellCommand::ToggleWorkspaceOverview,
            "workspace-overview-toggle should parse");
    require(parse_shell_command("relictombs-toggle") == ShellCommand::ToggleRelictombs,
            "relictombs-toggle should parse");
    require(parse_shell_command("restart") == ShellCommand::Restart,
            "restart should parse");
    require(parse_shell_command("quit") == ShellCommand::Quit,
            "quit should parse");
}

void rejects_unknown_shell_commands() {
    require(!realmheart::core::parse_shell_command(""), "empty command should be rejected");
    require(!realmheart::core::parse_shell_command("sidebar-toggle"),
            "unknown command should be rejected");
}

void maps_commands_to_stable_action_names() {
    using realmheart::core::ShellCommand;
    using realmheart::core::shell_action_name;

    require(shell_action_name(ShellCommand::ToggleRightSidebar) == "sidebar-right-toggle",
            "right sidebar action name should be stable");
    require(shell_action_name(ShellCommand::ToggleBar) == "bar-toggle",
            "bar action name should be stable");
    require(shell_action_name(ShellCommand::ToggleCharacter) == "character-toggle",
            "character action name should be stable");
    require(shell_action_name(ShellCommand::SetCharacterHairMode) == "character-hair-mode",
            "character hair mode action name should be stable");
    require(shell_action_name(ShellCommand::SetWallpaperPath) == "set-wallpaper-path",
            "direct wallpaper action name should be stable");
    require(shell_action_name(ShellCommand::SetWallpaperBackend) == "set-wallpaper-backend",
            "wallpaper backend action name should be stable");
    require(shell_action_name(ShellCommand::LaunchLauncherQuery) == "launch-launcher-query",
            "launcher query action name should be stable");
    require(shell_action_name(ShellCommand::ToggleWorkspaceOverview) == "workspace-overview-toggle",
            "workspace overview action name should be stable");
    require(shell_action_name(ShellCommand::ToggleRelictombs) == "relictombs-toggle",
            "Relictombs action name should be stable");
    require(shell_action_name(ShellCommand::Restart) == "restart",
            "restart action name should be stable");
    require(shell_action_name(ShellCommand::Quit) == "quit",
            "quit action name should be stable");
}

void identifies_commands_that_require_arguments() {
    using realmheart::core::ShellCommand;
    using realmheart::core::shell_command_requires_argument;

    require(shell_command_requires_argument(ShellCommand::SetCharacterHairMode),
            "character-hair-mode should require a mode argument");
    require(shell_command_requires_argument(ShellCommand::SetWallpaperPath),
            "set-wallpaper-path should require a path argument");
    require(shell_command_requires_argument(ShellCommand::SetWallpaperBackend),
            "set-wallpaper-backend should require a backend argument");
    require(shell_command_requires_argument(ShellCommand::LaunchLauncherQuery),
            "launch-launcher-query should require a query argument");
    require(!shell_command_requires_argument(ShellCommand::SetWallpaper),
            "interactive set-wallpaper should remain parameterless");
    require(!shell_command_requires_argument(ShellCommand::ToggleBar),
            "ordinary shell commands should remain parameterless");
    require(!shell_command_requires_argument(ShellCommand::ToggleCharacter),
            "character toggle should remain parameterless");
    require(!shell_command_requires_argument(ShellCommand::ToggleWorkspaceOverview),
            "workspace overview toggle should remain parameterless");
    require(!shell_command_requires_argument(ShellCommand::ToggleRelictombs),
            "Relictombs toggle should remain parameterless");
    require(!shell_command_requires_argument(ShellCommand::Restart),
            "restart should remain parameterless");
}

} // namespace

int main() {
    try {
        parses_supported_shell_commands();
        rejects_unknown_shell_commands();
        maps_commands_to_stable_action_names();
        identifies_commands_that_require_arguments();
    } catch (const std::exception& error) {
        std::cerr << "ShellCommandTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "ShellCommandTests passed\n";
    return 0;
}
