#include "services/LockRendererProcess.hpp"

#include <atomic>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace realmheart::services {

namespace {

std::string current_executable_path() {
    std::array<char, 4096> buffer{};
    const ssize_t length = ::readlink(
        "/proc/self/exe",
        buffer.data(),
        buffer.size() - 1
    );
    if (length <= 0) return {};
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

} // namespace

LockRendererProcess::LockRendererProcess(
    LockSessionProvider* session_provider,
    AuthSuccessCallback on_auth_success,
    VeilCompleteCallback on_veil_complete,
    ErrorCallback on_error
)
    : session_provider_(session_provider)
    , on_auth_success_(std::move(on_auth_success))
    , on_veil_complete_(std::move(on_veil_complete))
    , on_error_(std::move(on_error))
{}

LockRendererProcess::~LockRendererProcess() {
    stop();
}

std::string LockRendererProcess::resolve_renderer_path() const {
    const std::string executable = current_executable_path();
    if (executable.empty()) return {};
    return (
        std::filesystem::path(executable).parent_path() /
        "realmheart-lockscreen-renderer"
    ).string();
}

bool LockRendererProcess::start(const RendererConfig& config) {
    config_ = config;

    const std::string renderer = resolve_renderer_path();
    if (renderer.empty()) {
        on_renderer_error("Unable to resolve lockscreen renderer executable path");
        return false;
    }

    if (::access(renderer.c_str(), X_OK) != 0) {
        on_renderer_error("Lockscreen renderer is not executable: " + renderer +
                          ": " + std::strerror(errno));
        return false;
    }

    // Create the Unix socket pair for control communication.
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
        on_renderer_error("Failed to create socket pair: " +
                          std::string(std::strerror(errno)));
        return false;
    }

    const int parent_fd = sockets[0];
    const int child_fd = sockets[1];

    // Format args for the renderer.
    // The renderer always uses fd 3 (via dup2), so we just pass --stdio.
    std::vector<const char*> argv = {
        renderer.c_str(),
        "--stdio",
        nullptr
    };

    // Spawn the child process using fork+exec.
    pid_t pid = fork();
    if (pid < 0) {
        close(parent_fd);
        close(child_fd);
        on_renderer_error("Failed to fork renderer process: " +
                          std::string(std::strerror(errno)));
        return false;
    }

    if (pid == 0) {
        // Child process: close parent fd, dup child fd to fd 3.
        close(parent_fd);
        dup2(child_fd, 3);
        close(child_fd);

        execvp(renderer.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent process.
    pid_ = pid;
    socket_fd_ = parent_fd;
    close(child_fd);

    // Wait for READY handshake from the renderer.
    // The renderer sends "READY\n" after connecting to Wayland and
    // acquiring the session lock.
    char buf[256];
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(sizeof(buf) - 1)) {
        ssize_t n = read(socket_fd_, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) {
            on_renderer_error("Renderer process exited before READY handshake");
            stop();
            return false;
        }
        total += n;
        buf[total] = '\0';

        if (total >= 5 && strncmp(buf, "READY", 5) == 0) {
            renderer_ready_ = true;
            on_renderer_ready();
            return true;
        }
    }

    on_renderer_error("Renderer did not send READY within buffer");
    stop();
    return false;
}

void LockRendererProcess::send_command(const std::string& command) {
    if (!renderer_ready_ || socket_fd_ < 0) {
        pending_commands_.push_back(command);
        return;
    }

    std::string msg = command + "\n";
    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(msg.size())) {
        ssize_t n = write(socket_fd_, msg.data() + sent, msg.size() - sent);
        if (n < 0) {
            on_renderer_error("Failed to write command to renderer: " + command);
            return;
        }
        sent += n;
    }
}

void LockRendererProcess::on_renderer_ready() {
    // Send any pending commands.
    for (const auto& cmd : pending_commands_) {
        send_command(cmd);
    }
    pending_commands_.clear();
}

void LockRendererProcess::on_renderer_auth_success() {
    // The renderer verified the password via PAM and released the lock.
    // The parent just needs to update its state.
    if (on_auth_success_) {
        on_auth_success_();
    }
}

void LockRendererProcess::on_renderer_veil_complete() {
    // The renderer finished the handoff animation.
    if (on_veil_complete_) {
        on_veil_complete_();
    }
}

void LockRendererProcess::on_renderer_error(const std::string& message) {
    if (on_error_) {
        on_error_(message);
    }
}

void LockRendererProcess::on_renderer_exited() {
    pid_ = 0;
    socket_fd_ = -1;
    renderer_ready_ = false;
}

void LockRendererProcess::stop() {
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        int status = 0;
        waitpid(pid_, &status, 0);
        on_renderer_exited();
    }

    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }

    renderer_ready_ = false;
}

} // namespace realmheart::services
