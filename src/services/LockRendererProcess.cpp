#include "services/LockRendererProcess.hpp"

#include "core/Command.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace realmheart::services {

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

bool LockRendererProcess::start(const RendererConfig& config) {
    config_ = config;

    // Create the Unix socket pair for control communication.
    // Parent side: socket_fd_ (waits for renderer commands).
    // Child side: passed to renderer via fd inheritance.
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
        on_renderer_error("Failed to create socket pair");
        return false;
    }

    const int parent_fd = sockets[0];
    const int child_fd = sockets[1];

    // Build argv for the renderer subprocess.
    std::vector<const char*> argv = {
        config.renderer_path.c_str(),
        "--stdio",
        "--socket-fd",  // renderer reads its socket from fd 3
        "--seed",
        nullptr,  // placeholder for seed string
        "--background", config.background_asset.c_str(),
        "--rinia", config.rinia_asset.c_str(),
        nullptr
    };

    // Format seed as string.
    static thread_local std::string seed_buf;
    seed_buf = std::to_string(config.seed);
    argv[4] = seed_buf.c_str();

    // Spawn the child process using fork+exec (like PowerMenuProcess uses g_spawn).
    pid_t pid = fork();
    if (pid < 0) {
        close(parent_fd);
        close(child_fd);
        on_renderer_error("Failed to fork renderer process");
        return false;
    }

    if (pid == 0) {
        // Child process: close parent fd, dup child fd to fd 3.
        close(parent_fd);
        dup2(child_fd, 3);
        close(child_fd);

        // Exec the renderer binary.
        // In a real build this would be installed at the real path.
        execvp(config.renderer_path.c_str(), const_cast<char* const*>(argv.data()));
        // If exec fails:
        _exit(127);
    }

    // Parent process.
    pid_ = pid;
    socket_fd_ = parent_fd;
    close(child_fd);

    // Wait for READY handshake (similar to wallpaper-renderer).
    // Read from the socket until we get "READY\n".
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

void LockRendererProcess::on_socket_readable() {
    if (socket_fd_ < 0) return;

    char buf[256];
    ssize_t n = read(socket_fd_, buf, sizeof(buf) - 1);
    if (n <= 0) return;

    buf[n] = '\0';
    std::string message(buf);

    // Parse commands from renderer.
    // VEIL_COMPLETE — renderer finished the handoff resolve animation.
    // The parent must now call unlock_session() (composer unlock).
    // The session has been unlocked (PAM success) but the visual veil
    // just completed — this is the security-state-vs-visual-state boundary.
    if (message.find("VEIL_COMPLETE") == 0) {
        on_renderer_veil_complete();
        return;
    }

    // UNLOCK — (deprecated: renderer now sends VEIL_COMPLETE after handoff)
    // This is kept for backwards compatibility.
    if (message.find("UNLOCK") == 0) {
        on_renderer_auth_success();
        return;
    }

    // ERROR message.
    if (message.find("ERROR ") == 0) {
        on_renderer_error(message.substr(6));
        return;
    }

    // STATE message (renderer reports state changes).
    if (message.find("STATE ") == 0) {
        // Handle state updates if needed in the future.
        return;
    }
}

void LockRendererProcess::on_renderer_ready() {
    // Send any pending commands.
    for (const auto& cmd : pending_commands_) {
        send_command(cmd);
    }
    pending_commands_.clear();

    // Send the prophecy selection data.
    // In a real implementation, this would send the futures list,
    // seed, and surface handles via the socket + fd passing.
}

void LockRendererProcess::on_renderer_auth_success() {
    // The renderer verified the password via PAM.
    // The PARENT is the only thing authorized to call unlock_session().
    // In Phase 6, auth success only means the password was valid —
    // the session stays locked until the handoff veil completes.
    // (The on_auth_success_ callback fires but does NOT unlock the session.)
    if (on_auth_success_) {
        on_auth_success_();
    }
}

void LockRendererProcess::on_renderer_veil_complete() {
    // The renderer has finished the handoff resolve animation.
    // The session has been authenticated but visually locked —
    // now the parent releases the actual lock.
    if (session_provider_) {
        session_provider_->unlock_session();
    }
    if (on_auth_success_) {
        on_auth_success_();
    }
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
        // Reap the child.
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
