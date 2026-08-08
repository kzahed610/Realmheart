#include "ui/worldscar/WorldscarProcess.hpp"

#include <glib-unix.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <utility>
#include <unistd.h>

namespace realmheart::ui::worldscar {
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

bool send_all(int fd, const char* data, std::size_t size) noexcept {
    while (size > 0) {
        const ssize_t written = ::send(fd, data, size, MSG_NOSIGNAL);
        if (written > 0) {
            data += written;
            size -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}


} // namespace

WorldscarProcess::~WorldscarProcess() {
    shutdown();
}

bool WorldscarProcess::warm() {
    if (running()) return true;
    return launch();
}

void WorldscarProcess::prepare(std::string current_wallpaper) noexcept {
    if (current_wallpaper.empty() || session_active_) return;
    if (!warm()) return;

    pending_prepare_wallpaper_ = std::move(current_wallpaper);
    if (ready_) send_pending_prepare();
}

bool WorldscarProcess::open(
    std::string current_wallpaper,
    ResultCallback callback
) {
    if (session_active_ || pending_open_) return false;
    if (current_wallpaper.empty()) return false;
    if (!warm()) return false;

    result_callback_ = std::move(callback);
    pending_current_wallpaper_ = std::move(current_wallpaper);
    pending_open_ = true;
    session_active_ = true;
    if (ready_) send_pending_open();
    return true;
}

void WorldscarProcess::close() noexcept {
    if (!session_active_) return;

    if (pending_open_ && !ready_) {
        pending_open_ = false;
        pending_prepare_wallpaper_.clear();
        pending_current_wallpaper_.clear();
        session_active_ = false;
        auto callback = std::move(result_callback_);
        result_callback_ = {};
        if (callback) {
            callback({realmheart::worldscar::WorldscarResultKind::Cancel, {}});
        }
        return;
    }

    static_cast<void>(send_command({
        realmheart::worldscar::WorldscarCommandKind::Close,
        {}
    }));
}

void WorldscarProcess::apply_prepared() noexcept {
    if (!session_active_) return;
    if (!send_command({
            realmheart::worldscar::WorldscarCommandKind::ApplyPrepared,
            {}
        })) {
        handle_result({
            realmheart::worldscar::WorldscarResultKind::Error,
            "unable to acknowledge prepared wallpaper to Worldscar"
        });
    }
}

void WorldscarProcess::apply_committed() noexcept {
    if (!session_active_) return;
    if (!send_command({
            realmheart::worldscar::WorldscarCommandKind::ApplyCommitted,
            {}
        })) {
        handle_result({
            realmheart::worldscar::WorldscarResultKind::Error,
            "unable to acknowledge committed wallpaper to Worldscar"
        });
    }
}

void WorldscarProcess::apply_failed(std::string diagnostic) noexcept {
    if (!session_active_) return;
    if (diagnostic.empty()) diagnostic = "wallpaper backend apply failed";
    if (!send_command({
            realmheart::worldscar::WorldscarCommandKind::ApplyFailed,
            std::move(diagnostic)
        })) {
        handle_result({
            realmheart::worldscar::WorldscarResultKind::Error,
            "unable to notify Worldscar about wallpaper backend failure"
        });
    }
}

void WorldscarProcess::refresh_library() noexcept {
    if (!running() || !ready_ || session_active_) return;
    static_cast<void>(send_command({
        realmheart::worldscar::WorldscarCommandKind::Refresh,
        {}
    }));
}

void WorldscarProcess::shutdown() noexcept {
    ready_ = false;
    pending_open_ = false;
    session_active_ = false;
    pending_prepare_wallpaper_.clear();
    pending_current_wallpaper_.clear();
    result_callback_ = {};

    if (output_watch_id_ != 0) {
        g_source_remove(output_watch_id_);
        output_watch_id_ = 0;
    }
    if (child_watch_id_ != 0) {
        g_source_remove(child_watch_id_);
        child_watch_id_ = 0;
    }

    if (control_fd_ >= 0) {
        ::close(control_fd_);
        control_fd_ = -1;
    }

    if (child_pid_ != 0) {
        ::kill(child_pid_, SIGTERM);
        int status = 0;
        while (::waitpid(child_pid_, &status, 0) < 0 && errno == EINTR) {
        }
        g_spawn_close_pid(child_pid_);
        child_pid_ = 0;
    }
}

bool WorldscarProcess::ready() const noexcept {
    return ready_;
}

bool WorldscarProcess::session_active() const noexcept {
    return session_active_;
}

bool WorldscarProcess::running() const noexcept {
    return child_pid_ != 0;
}

bool WorldscarProcess::launch() {
    const std::string helper = helper_executable();
    if (helper.empty()) {
        std::cerr << "[WorldscarProcess] unable to resolve helper executable\n";
        return false;
    }
    if (::access(helper.c_str(), X_OK) != 0) {
        std::cerr << "[WorldscarProcess] helper is not executable: "
                  << helper << ": " << std::strerror(errno) << '\n';
        return false;
    }

    std::array<gchar*, 3> arguments{
        const_cast<gchar*>(helper.c_str()),
        const_cast<gchar*>("--stdio"),
        nullptr,
    };

    // One full-duplex Unix socket is mapped to both stdin and stdout in the
    // helper. This keeps the warm control protocol bidirectional while
    // allowing MSG_NOSIGNAL on writes, so a dead helper can never SIGPIPE the
    // persistent Realmheart shell.
    std::array<int, 2> control_sockets{-1, -1};
    if (::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_CLOEXEC,
            0,
            control_sockets.data()) != 0) {
        std::cerr << "[WorldscarProcess] unable to create control socket: "
                  << std::strerror(errno) << '\n';
        return false;
    }

    GError* error = nullptr;
    GPid child_pid = 0;
    const gboolean spawned = g_spawn_async_with_fds(
        nullptr,
        arguments.data(),
        nullptr,
        G_SPAWN_DO_NOT_REAP_CHILD,
        nullptr,
        nullptr,
        &child_pid,
        control_sockets[1],
        control_sockets[1],
        STDERR_FILENO,
        &error
    );

    ::close(control_sockets[1]);
    if (!spawned) {
        ::close(control_sockets[0]);
        std::cerr << "[WorldscarProcess] unable to launch warm helper: "
                  << (error != nullptr && error->message != nullptr
                      ? error->message
                      : "unknown error")
                  << '\n';
        g_clear_error(&error);
        return false;
    }

    child_pid_ = child_pid;
    control_fd_ = control_sockets[0];
    ready_ = false;
    output_buffer_.clear();
    output_watch_id_ = g_unix_fd_add(
        control_fd_,
        static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
        &WorldscarProcess::stdout_ready_callback,
        this
    );
    child_watch_id_ = g_child_watch_add(
        child_pid_,
        &WorldscarProcess::child_watch_callback,
        this
    );

    std::cerr << "[WorldscarProcess] warm helper started: pid="
              << child_pid_ << '\n';
    return true;
}

bool WorldscarProcess::send_command(
    const realmheart::worldscar::WorldscarCommand& command
) noexcept {
    if (control_fd_ < 0) return false;
    const std::string encoded =
        realmheart::worldscar::serialize_worldscar_command(command);
    if (send_all(control_fd_, encoded.data(), encoded.size())) return true;

    std::cerr << "[WorldscarProcess] control write failed: "
              << std::strerror(errno) << '\n';
    return false;
}

void WorldscarProcess::send_pending_prepare() noexcept {
    if (!ready_ || pending_prepare_wallpaper_.empty()) return;
    if (session_active_ && !pending_open_) return;
    if (!send_command({
            realmheart::worldscar::WorldscarCommandKind::Prepare,
            pending_prepare_wallpaper_
        })) {
        std::cerr << "[WorldscarProcess] unable to send candidate prepare command\n";
        return;
    }
    pending_prepare_wallpaper_.clear();
}

void WorldscarProcess::send_pending_open() noexcept {
    if (!ready_ || !pending_open_ || pending_current_wallpaper_.empty()) return;
    if (!send_command({
            realmheart::worldscar::WorldscarCommandKind::Open,
            pending_current_wallpaper_
        })) {
        handle_result({
            realmheart::worldscar::WorldscarResultKind::Error,
            "unable to send Worldscar open command"
        });
        return;
    }
    pending_open_ = false;
    pending_current_wallpaper_.clear();
}

void WorldscarProcess::consume_output() noexcept {
    if (control_fd_ < 0) return;

    std::array<char, 4096> buffer{};
    ssize_t count = -1;
    do {
        count = ::recv(control_fd_, buffer.data(), buffer.size(), 0);
    } while (count < 0 && errno == EINTR);

    if (count > 0) {
        output_buffer_.append(buffer.data(), static_cast<std::size_t>(count));
    } else if (count < 0) {
        std::cerr << "[WorldscarProcess] helper output read failed: "
                  << std::strerror(errno) << '\n';
    }

    std::size_t newline = 0;
    while ((newline = output_buffer_.find('\n')) != std::string::npos) {
        std::string line = output_buffer_.substr(0, newline + 1);
        output_buffer_.erase(0, newline + 1);
        handle_line(std::move(line));
    }
}

void WorldscarProcess::handle_line(std::string line) noexcept {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    if (line == "READY") {
        const bool first_ready = !ready_;
        ready_ = true;
        if (first_ready) {
            std::cerr << "[WorldscarProcess] warm helper ready\n";
        }
        send_pending_prepare();
        send_pending_open();
        return;
    }

    const auto result = realmheart::worldscar::parse_worldscar_result(line);
    if (!result) {
        if (!line.empty()) {
            std::cerr << "[WorldscarProcess] ignored malformed helper output\n";
        }
        return;
    }
    handle_result(*result);
}

void WorldscarProcess::handle_result(
    realmheart::worldscar::WorldscarResult result
) noexcept {
    const bool terminal =
        result.kind != realmheart::worldscar::WorldscarResultKind::Apply &&
        result.kind != realmheart::worldscar::WorldscarResultKind::Commit;

    if (terminal) {
        session_active_ = false;
        pending_open_ = false;
        pending_current_wallpaper_.clear();
    }

    if (!result_callback_) return;
    if (terminal) {
        auto callback = std::move(result_callback_);
        result_callback_ = {};
        callback(std::move(result));
    } else {
        result_callback_(std::move(result));
    }
}

void WorldscarProcess::reap_child(int status) noexcept {
    if (output_watch_id_ != 0) {
        g_source_remove(output_watch_id_);
        output_watch_id_ = 0;
    }
    if (control_fd_ >= 0) {
        ::close(control_fd_);
        control_fd_ = -1;
    }

    const GPid completed_pid = child_pid_;
    child_pid_ = 0;
    child_watch_id_ = 0;
    ready_ = false;
    pending_prepare_wallpaper_.clear();
    if (completed_pid != 0) g_spawn_close_pid(completed_pid);

    if (session_active_ && result_callback_) {
        auto callback = std::move(result_callback_);
        result_callback_ = {};
        session_active_ = false;
        pending_open_ = false;
        pending_prepare_wallpaper_.clear();
        pending_current_wallpaper_.clear();
        callback({
            realmheart::worldscar::WorldscarResultKind::Error,
            "Worldscar warm helper exited during an active session"
        });
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        std::cerr << "[WorldscarProcess] warm helper exited cleanly\n";
        return;
    }
    if (WIFSIGNALED(status)) {
        std::cerr << "[WorldscarProcess] warm helper terminated by signal "
                  << WTERMSIG(status) << '\n';
        return;
    }
    std::cerr << "[WorldscarProcess] warm helper exited with status "
              << status << '\n';
}

std::string WorldscarProcess::helper_executable() const {
    const std::string executable = current_executable_path();
    if (executable.empty()) return {};
    return (
        std::filesystem::path(executable).parent_path() /
        "realmheart-worldscar-renderer"
    ).string();
}

gboolean WorldscarProcess::stdout_ready_callback(
    gint,
    GIOCondition condition,
    gpointer data
) {
    auto* self = static_cast<WorldscarProcess*>(data);
    if (self == nullptr) return G_SOURCE_REMOVE;

    self->consume_output();
    if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
        self->output_watch_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void WorldscarProcess::child_watch_callback(
    GPid,
    gint status,
    gpointer data
) {
    auto* self = static_cast<WorldscarProcess*>(data);
    if (self != nullptr) self->reap_child(status);
}

} // namespace realmheart::ui::worldscar
