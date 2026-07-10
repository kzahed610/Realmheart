#include "core/ShellCommand.hpp"
#include "core/ShellControl.hpp"

#include <iostream>
#include <stdexcept>

int main() {
    const auto result = realmheart::core::send_shell_command(
        realmheart::core::ShellCommand::ToggleBar
    );

    if (result != realmheart::core::ShellControlResult::NotRunning) {
        std::cerr << "ShellRuntimeTests failed: expected NotRunning when no primary shell exists\n";
        return 1;
    }

    std::cout << "ShellRuntimeTests passed\n";
    return 0;
}
