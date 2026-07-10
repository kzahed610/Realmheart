#pragma once

#include <optional>
#include <string_view>

namespace realmheart::core {

enum class ShellCommand {
    ToggleRightSidebar,
    ToggleBar,
    ShowOSDVolume,
    ShowOSDBrightness,
    Quit,
};

std::optional<ShellCommand> parse_shell_command(std::string_view name);
std::string_view shell_action_name(ShellCommand command);

} // namespace realmheart::core
