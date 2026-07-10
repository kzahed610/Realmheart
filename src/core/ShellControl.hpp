#pragma once

#include "core/ShellCommand.hpp"

#include <string_view>

namespace realmheart::core {

enum class ShellControlResult {
    Delivered = 0,
    NotRunning = 1,
    RegistrationFailed = 2,
    ActionUnavailable = 3,
};

std::string_view shell_application_id();
ShellControlResult send_shell_command(ShellCommand command);

} // namespace realmheart::core
