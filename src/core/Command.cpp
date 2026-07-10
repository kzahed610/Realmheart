#include "core/Command.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace realmheart::core {
namespace {

using Clock = std::chrono::steady_clock;

CommandResult error_result(CommandStatus status, std::string error) {
    CommandResult result;
    result.status = status;
    result.error = std::move(error);
    return result;
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void signal_process_group(pid_t child, int signal_number) {
    if (::kill(-child, signal_number) != 0 && errno == ESRCH) {
        ::kill(child, signal_number);
    }
}

void drain_output(int& fd, CommandResult& result, std::size_t max_output_bytes, std::string& io_error) {
    std::array<char, 4096> buffer{};
    for (int reads = 0; reads < 32 && fd >= 0; ++reads) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count > 0) {
            const auto available = max_output_bytes > result.output.size()
                ? max_output_bytes - result.output.size()
                : 0;
            const auto captured = std::min<std::size_t>(available, static_cast<std::size_t>(count));
            result.output.append(buffer.data(), captured);
            if (captured < static_cast<std::size_t>(count)) result.truncated = true;
            continue;
        }
        if (count == 0) {
            close_fd(fd);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        io_error = std::string("read failed: ") + std::strerror(errno);
        close_fd(fd);
        return;
    }
}

void drain_exec_error(
    int& fd,
    std::array<unsigned char, sizeof(int)>& buffer,
    std::size_t& bytes_read,
    std::string& io_error
) {
    while (fd >= 0 && bytes_read < buffer.size()) {
        const ssize_t count = ::read(fd, buffer.data() + bytes_read, buffer.size() - bytes_read);
        if (count > 0) {
            bytes_read += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) {
            close_fd(fd);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        io_error = std::string("exec status read failed: ") + std::strerror(errno);
        close_fd(fd);
        return;
    }
}

std::string bounded_with_ellipsis(std::string text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    if (max_bytes <= 3) return std::string(max_bytes, '.');
    text.resize(max_bytes - 3);
    text += "...";
    return text;
}

} // namespace

std::string trim(std::string text) {
    const auto start = text.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\n\r");
    return text.substr(start, end - start + 1);
}

std::optional<std::string> find_in_path(const std::string& name) {
    if (name.empty()) return std::nullopt;
    if (name.find('/') != std::string::npos) {
        if (::access(name.c_str(), X_OK) == 0) return name;
        return std::nullopt;
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;

    std::stringstream paths(path_env);
    std::string dir;
    while (std::getline(paths, dir, ':')) {
        if (dir.empty()) dir = ".";
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate.string();
    }
    return std::nullopt;
}

bool command_exists(const std::string& name) {
    return find_in_path(name).has_value();
}

bool CommandResult::succeeded() const noexcept {
    return status == CommandStatus::Exited && exit_code == 0;
}

CommandResult run_capture(const std::vector<std::string>& argv, const CommandOptions& options) {
    if (argv.empty() || argv.front().empty()) {
        return error_result(CommandStatus::InvalidArguments, "command argv is empty");
    }
    if (options.deadline <= std::chrono::milliseconds::zero()) {
        return error_result(CommandStatus::InvalidArguments, "command deadline must be positive");
    }
    if (options.cancelled && options.cancelled()) {
        return error_result(CommandStatus::Cancelled, "command cancelled before spawn");
    }

    std::vector<char*> exec_argv;
    exec_argv.reserve(argv.size() + 1);
    for (const auto& argument : argv) exec_argv.push_back(const_cast<char*>(argument.c_str()));
    exec_argv.push_back(nullptr);

    int output_pipe[2] = {-1, -1};
    int exec_pipe[2] = {-1, -1};
    if (::pipe2(output_pipe, O_CLOEXEC) != 0) {
        return error_result(CommandStatus::SystemError, std::string("pipe2 failed: ") + std::strerror(errno));
    }
    if (::pipe2(exec_pipe, O_CLOEXEC) != 0) {
        const auto error = std::string("exec pipe2 failed: ") + std::strerror(errno);
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        return error_result(CommandStatus::SystemError, error);
    }

    const pid_t child = ::fork();
    if (child < 0) {
        const auto error = std::string("fork failed: ") + std::strerror(errno);
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        ::close(exec_pipe[0]);
        ::close(exec_pipe[1]);
        return error_result(CommandStatus::SpawnFailed, error);
    }

    if (child == 0) {
        ::close(output_pipe[0]);
        ::close(exec_pipe[0]);
        ::setpgid(0, 0);

        if (::dup2(output_pipe[1], STDOUT_FILENO) < 0 || ::dup2(output_pipe[1], STDERR_FILENO) < 0) {
            const int child_errno = errno;
            static_cast<void>(::write(exec_pipe[1], &child_errno, sizeof(child_errno)));
            _exit(127);
        }
        ::close(output_pipe[1]);

        ::execvp(exec_argv[0], exec_argv.data());
        const int child_errno = errno;
        static_cast<void>(::write(exec_pipe[1], &child_errno, sizeof(child_errno)));
        _exit(127);
    }

    ::close(output_pipe[1]);
    output_pipe[1] = -1;
    ::close(exec_pipe[1]);
    exec_pipe[1] = -1;
    static_cast<void>(::setpgid(child, child));

    CommandResult result;
    result.status = CommandStatus::SystemError;
    if (!set_nonblocking(output_pipe[0]) || !set_nonblocking(exec_pipe[0])) {
        result.error = std::string("fcntl failed: ") + std::strerror(errno);
        signal_process_group(child, SIGKILL);
        static_cast<void>(::waitpid(child, nullptr, 0));
        close_fd(output_pipe[0]);
        close_fd(exec_pipe[0]);
        return result;
    }

    const auto started = Clock::now();
    const auto deadline = started + options.deadline;
    const auto grace = std::max(options.terminate_grace, std::chrono::milliseconds::zero());
    auto escalation_at = deadline;
    auto final_wait_at = deadline;
    bool terminating = false;
    bool kill_sent = false;
    bool child_reaped = false;
    int wait_status = 0;
    std::string io_error;
    std::array<unsigned char, sizeof(int)> exec_error_buffer{};
    std::size_t exec_error_bytes = 0;

    while (!child_reaped) {
        pollfd descriptors[2] = {
            {output_pipe[0], POLLIN | POLLHUP | POLLERR, 0},
            {exec_pipe[0], POLLIN | POLLHUP | POLLERR, 0},
        };
        static_cast<void>(::poll(descriptors, 2, 20));

        drain_output(output_pipe[0], result, options.max_output_bytes, io_error);
        drain_exec_error(exec_pipe[0], exec_error_buffer, exec_error_bytes, io_error);

        const pid_t waited = ::waitpid(child, &wait_status, WNOHANG);
        if (waited == child) {
            child_reaped = true;
            drain_output(output_pipe[0], result, options.max_output_bytes, io_error);
            drain_exec_error(exec_pipe[0], exec_error_buffer, exec_error_bytes, io_error);
            break;
        }
        if (waited < 0 && errno != EINTR) {
            io_error = std::string("waitpid failed: ") + std::strerror(errno);
            break;
        }

        const auto now = Clock::now();
        const bool cancelled = options.cancelled && options.cancelled();
        if (!terminating && (cancelled || now >= deadline)) {
            result.status = cancelled ? CommandStatus::Cancelled : CommandStatus::TimedOut;
            result.error = cancelled ? "command cancelled" : "command timed out";
            signal_process_group(child, SIGTERM);
            terminating = true;
            escalation_at = now + grace;
            final_wait_at = escalation_at + std::chrono::milliseconds(250);
        }
        if (terminating && !kill_sent && now >= escalation_at) {
            signal_process_group(child, SIGKILL);
            kill_sent = true;
        }
        if (kill_sent && now >= final_wait_at) {
            result.error += "; child termination could not be confirmed";
            break;
        }
    }

    if (!child_reaped) {
        signal_process_group(child, SIGKILL);
        const pid_t waited = ::waitpid(child, &wait_status, WNOHANG);
        child_reaped = waited == child;
    }

    close_fd(output_pipe[0]);
    close_fd(exec_pipe[0]);
    result.output = trim(std::move(result.output));

    int exec_errno = 0;
    if (exec_error_bytes == sizeof(exec_errno)) {
        std::memcpy(&exec_errno, exec_error_buffer.data(), sizeof(exec_errno));
        result.status = CommandStatus::SpawnFailed;
        result.exit_code = 127;
        result.error = std::string("execvp failed: ") + std::strerror(exec_errno);
        return result;
    }
    if (!io_error.empty() && result.status != CommandStatus::TimedOut && result.status != CommandStatus::Cancelled) {
        result.status = CommandStatus::SystemError;
        result.error = std::move(io_error);
        return result;
    }
    if (result.status == CommandStatus::TimedOut || result.status == CommandStatus::Cancelled) {
        if (child_reaped && WIFSIGNALED(wait_status)) result.term_signal = WTERMSIG(wait_status);
        return result;
    }
    if (!child_reaped) {
        result.status = CommandStatus::SystemError;
        if (result.error.empty()) result.error = "child termination could not be confirmed";
        return result;
    }
    if (WIFEXITED(wait_status)) {
        result.status = CommandStatus::Exited;
        result.exit_code = WEXITSTATUS(wait_status);
        return result;
    }
    if (WIFSIGNALED(wait_status)) {
        result.status = CommandStatus::Signaled;
        result.term_signal = WTERMSIG(wait_status);
        result.error = "command terminated by signal " + std::to_string(result.term_signal);
        return result;
    }

    result.status = CommandStatus::SystemError;
    result.error = "child exited with an unknown wait status";
    return result;
}

std::string sanitize_command_detail(std::string_view text, std::size_t max_bytes) {
    std::string sanitized;
    sanitized.reserve(std::min(text.size(), max_bytes));
    bool pending_space = false;

    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte == 0x1b) {
            ++index;
            if (index < text.size() && text[index] == '[') {
                ++index;
                while (index < text.size()) {
                    const auto control = static_cast<unsigned char>(text[index++]);
                    if (control >= 0x40 && control <= 0x7e) break;
                }
            } else if (index < text.size() && text[index] == ']') {
                ++index;
                while (index < text.size()) {
                    if (text[index] == '\a') {
                        ++index;
                        break;
                    }
                    if (text[index] == 0x1b && index + 1 < text.size() && text[index + 1] == '\\') {
                        index += 2;
                        break;
                    }
                    ++index;
                }
            }
            continue;
        }
        ++index;

        if (std::isspace(byte)) {
            pending_space = !sanitized.empty();
            continue;
        }
        if (byte < 0x20 || byte == 0x7f) continue;
        if (pending_space) {
            sanitized.push_back(' ');
            pending_space = false;
        }
        sanitized.push_back(static_cast<char>(byte));
    }

    return bounded_with_ellipsis(std::move(sanitized), max_bytes);
}

std::string command_failure_detail(
    const CommandResult& result,
    std::string_view fallback,
    std::size_t max_bytes
) {
    std::string detail;
    switch (result.status) {
    case CommandStatus::TimedOut:
        detail = "command timed out";
        break;
    case CommandStatus::Cancelled:
        detail = "command cancelled";
        break;
    case CommandStatus::SpawnFailed:
    case CommandStatus::InvalidArguments:
    case CommandStatus::SystemError:
    case CommandStatus::Signaled:
        detail = result.error;
        break;
    case CommandStatus::Exited:
        detail = result.output;
        break;
    }
    if (detail.empty()) detail = std::string(fallback);
    if (result.truncated) detail += " [output truncated]";
    return sanitize_command_detail(detail, max_bytes);
}

} // namespace realmheart::core
