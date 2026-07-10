#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realmheart::core {

enum class CommandStatus {
    Exited,
    Signaled,
    TimedOut,
    Cancelled,
    SpawnFailed,
    InvalidArguments,
    SystemError,
};

struct CommandOptions {
    std::chrono::milliseconds deadline{1500};
    std::chrono::milliseconds terminate_grace{100};
    std::size_t max_output_bytes = 64 * 1024;
    std::function<bool()> cancelled;
};

struct CommandResult {
    CommandStatus status = CommandStatus::InvalidArguments;
    int exit_code = -1;
    int term_signal = 0;
    std::string output;
    std::string error;
    bool truncated = false;

    [[nodiscard]] bool succeeded() const noexcept;
};

bool command_exists(const std::string& name);
std::optional<std::string> find_in_path(const std::string& name);
CommandResult run_capture(const std::vector<std::string>& argv, const CommandOptions& options = {});
std::string sanitize_command_detail(std::string_view text, std::size_t max_bytes = 160);
std::string command_failure_detail(
    const CommandResult& result,
    std::string_view fallback,
    std::size_t max_bytes = 160
);
std::string trim(std::string text);

} // namespace realmheart::core
