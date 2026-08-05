#include "ui/powermenu/PowerMenuProcess.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace realmheart::ui::powermenu {
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

double sanitize_origin(double value, double fallback) noexcept {
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, 0.0, 1.0);
}

std::string format_origin(double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(8) << value;
    return stream.str();
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

PowerMenuProcess::~PowerMenuProcess() {
    close();

    if (child_watch_id_ != 0) {
        g_source_remove(child_watch_id_);
        child_watch_id_ = 0;
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

void PowerMenuProcess::toggle(
    double normalized_origin_x,
    double normalized_origin_y
) {
    if (running()) {
        request_close();
        return;
    }
    static_cast<void>(launch(normalized_origin_x, normalized_origin_y));
}

void PowerMenuProcess::close() noexcept {
    if (!running()) return;
    request_close();
}

bool PowerMenuProcess::running() const noexcept {
    return child_pid_ != 0;
}

bool PowerMenuProcess::launch(
    double normalized_origin_x,
    double normalized_origin_y
) {
    const std::string helper = helper_executable();
    if (helper.empty()) {
        std::cerr << "[PowerMenuProcess] unable to resolve helper executable\n";
        return false;
    }

    if (::access(helper.c_str(), X_OK) != 0) {
        std::cerr << "[PowerMenuProcess] helper is not executable: "
                  << helper << ": " << std::strerror(errno) << '\n';
        return false;
    }

    const std::string origin_x = format_origin(
        sanitize_origin(normalized_origin_x, 24.0 / 1920.0)
    );
    const std::string origin_y = format_origin(
        sanitize_origin(normalized_origin_y, 1048.0 / 1080.0)
    );

    std::array<gchar*, 6> arguments{
        const_cast<gchar*>(helper.c_str()),
        const_cast<gchar*>("--origin-x"),
        const_cast<gchar*>(origin_x.c_str()),
        const_cast<gchar*>("--origin-y"),
        const_cast<gchar*>(origin_y.c_str()),
        nullptr,
    };

    std::array<int, 2> control_sockets{-1, -1};
    if (::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_CLOEXEC,
            0,
            control_sockets.data()) != 0) {
        std::cerr << "[PowerMenuProcess] unable to create control socket: "
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
        STDOUT_FILENO,
        STDERR_FILENO,
        &error
    );

    ::close(control_sockets[1]);
    if (!spawned) {
        ::close(control_sockets[0]);
        std::cerr << "[PowerMenuProcess] unable to launch helper: "
                  << (error != nullptr && error->message != nullptr
                      ? error->message
                      : "unknown error")
                  << '\n';
        g_clear_error(&error);
        return false;
    }

    child_pid_ = child_pid;
    control_fd_ = control_sockets[0];
    child_watch_id_ = g_child_watch_add(
        child_pid_,
        &PowerMenuProcess::child_watch_callback,
        this
    );

    std::cerr << "[PowerMenuProcess] helper started: pid=" << child_pid_
              << " origin=" << origin_x << ',' << origin_y << '\n';
    return true;
}

void PowerMenuProcess::request_close() noexcept {
    if (control_fd_ < 0) return;

    constexpr char command[] = "close\n";
    if (!send_all(control_fd_, command, sizeof(command) - 1)) {
        std::cerr << "[PowerMenuProcess] unable to send close command: "
                  << std::strerror(errno) << '\n';
    }

    // Closing the pipe guarantees that a helper which misses the textual
    // command still observes EOF and begins its closing animation.
    ::close(control_fd_);
    control_fd_ = -1;
}

void PowerMenuProcess::reap_child(int status) noexcept {
    if (control_fd_ >= 0) {
        ::close(control_fd_);
        control_fd_ = -1;
    }

    const GPid completed_pid = child_pid_;
    child_pid_ = 0;
    child_watch_id_ = 0;
    if (completed_pid != 0) g_spawn_close_pid(completed_pid);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        std::cerr << "[PowerMenuProcess] helper exited cleanly\n";
        return;
    }
    if (WIFSIGNALED(status)) {
        std::cerr << "[PowerMenuProcess] helper terminated by signal "
                  << WTERMSIG(status) << '\n';
        return;
    }
    std::cerr << "[PowerMenuProcess] helper exited with status " << status << '\n';
}

std::string PowerMenuProcess::helper_executable() const {
    const std::string executable = current_executable_path();
    if (executable.empty()) return {};
    return (
        std::filesystem::path(executable).parent_path() /
        "realmheart-power-menu-renderer"
    ).string();
}

void PowerMenuProcess::child_watch_callback(
    GPid,
    gint status,
    gpointer data
) {
    auto* self = static_cast<PowerMenuProcess*>(data);
    if (self != nullptr) self->reap_child(status);
}

} // namespace realmheart::ui::powermenu
