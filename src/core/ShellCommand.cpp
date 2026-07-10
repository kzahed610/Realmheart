#include "core/ShellCommand.hpp"

namespace realmheart::core {

std::optional<ShellCommand> parse_shell_command(std::string_view name) {
    if (name == "sidebar-right-toggle") return ShellCommand::ToggleRightSidebar;
    if (name == "bar-toggle") return ShellCommand::ToggleBar;
    if (name == "osd-volume") return ShellCommand::ShowOSDVolume;
    if (name == "osd-brightness") return ShellCommand::ShowOSDBrightness;
    if (name == "quit") return ShellCommand::Quit;
    return std::nullopt;
}

std::string_view shell_action_name(ShellCommand command) {
    switch (command) {
    case ShellCommand::ToggleRightSidebar: return "sidebar-right-toggle";
    case ShellCommand::ToggleBar: return "bar-toggle";
    case ShellCommand::ShowOSDVolume: return "osd-volume";
    case ShellCommand::ShowOSDBrightness: return "osd-brightness";
    case ShellCommand::Quit: return "quit";
    }
    return {};
}

} // namespace realmheart::core
