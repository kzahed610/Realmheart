#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
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
    std::optional<std::chrono::steady_clock::time_point> deadline_at;
    std::chrono::milliseconds terminate_grace{100};
    std::size_t max_output_bytes = 64 * 1024;
    bool separate_stderr = false;
    std::optional<std::string> stdin_data;
    std::optional<std::string> working_directory;
    std::function<bool()> cancelled;
};

struct BackgroundProcess {
    pid_t pid = -1;

    BackgroundProcess() = default;
    explicit BackgroundProcess(pid_t process_id) : pid(process_id) {}
    BackgroundProcess(const BackgroundProcess&) = delete;
    BackgroundProcess& operator=(const BackgroundProcess&) = delete;
    BackgroundProcess(BackgroundProcess&& other) noexcept : pid(other.pid) { other.pid = -1; }
    BackgroundProcess& operator=(BackgroundProcess&& other) noexcept {
        if (this != &other) {
            pid = other.pid;
            other.pid = -1;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return pid > 0; }
    bool stop(std::chrono::milliseconds timeout = std::chrono::milliseconds(350)) noexcept;
};

struct CommandResult {
    CommandStatus status = CommandStatus::InvalidArguments;
    int exit_code = -1;
    int term_signal = 0;
    std::string output;
    std::string standard_error;
    std::string error;
    bool truncated = false;

    [[nodiscard]] bool succeeded() const noexcept;
};

bool command_exists(const std::string& name);
std::optional<std::string> find_in_path(const std::string& name);
CommandResult run_capture(const std::vector<std::string>& argv, const CommandOptions& options = {});
bool run_background(const std::vector<std::string>& argv);
std::optional<BackgroundProcess> run_background_tracked(const std::vector<std::string>& argv);
std::string sanitize_command_detail(std::string_view text, std::size_t max_bytes = 160);
std::string command_failure_detail(
    const CommandResult& result,
    std::string_view fallback,
    std::size_t max_bytes = 160
);
std::string trim(std::string text);

} // namespace realmheart::core
