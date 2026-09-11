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
    LockReady = 6,
    LockFailed = 7,
};

std::string_view shell_application_id();
// The control plane intentionally trusts any peer on this user's session bus.
// It is a same-user desktop convenience channel, not an authentication
// boundary or a cross-user privilege mechanism.
ShellControlResult send_shell_command(ShellCommand command, std::string_view argument = {});
// Wait for the persistent shell's native lock acknowledgement. The reply is
// carried by a private D-Bus method, not a forgeable runtime status file.
ShellControlResult request_shell_lock(std::string_view request_token = {});

} // namespace realmheart::core
