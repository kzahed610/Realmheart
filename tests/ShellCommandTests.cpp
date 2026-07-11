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
    require(parse_shell_command("start-recording") == ShellCommand::StartRecording,
            "start-recording should parse");
    require(parse_shell_command("stop-recording") == ShellCommand::StopRecording,
            "stop-recording should parse");
    require(parse_shell_command("toggle-notes") == ShellCommand::ToggleNotes,
            "toggle-notes should parse");
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
    require(shell_action_name(ShellCommand::Quit) == "quit",
            "quit action name should be stable");
}

} // namespace

int main() {
    try {
        parses_supported_shell_commands();
        rejects_unknown_shell_commands();
        maps_commands_to_stable_action_names();
    } catch (const std::exception& error) {
        std::cerr << "ShellCommandTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "ShellCommandTests passed\n";
    return 0;
}
