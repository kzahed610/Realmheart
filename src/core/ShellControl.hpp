#pragma once

#include "core/ShellCommand.hpp"

#include <string_view>

namespace realmheart::core {

enum class ShellControlResult {
    Delivered = 0,
    NotRunning = 1,
    RegistrationFailed = 2,
    ActionUnavailable = 3,
    InvalidArgument = 4,
    DeliveryFailed = 5,
};

std::string_view shell_application_id();
// The control plane intentionally trusts any peer on this user's session bus.
// It is a same-user desktop convenience channel, not an authentication
// boundary or a cross-user privilege mechanism.
ShellControlResult send_shell_command(ShellCommand command, std::string_view argument = {});

} // namespace realmheart::core
