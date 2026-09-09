#include "ui/powermenu/PowerMenuProcess.hpp"

#include "ui/powermenu/PowerMenuContracts.hpp"

#include <glib-unix.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace realmheart::ui::powermenu {
namespace {

// The renderer's normal close transition lasts 1.05 s. Keep the
// escalation bounded, but give that lifecycle a small scheduling margin.
constexpr guint kCloseGraceMs = kPowerMenuCloseGraceMs;
constexpr guint kTerminateGraceMs = 250;
constexpr guint kStartupWatchdogMs = 1500;

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

void signal_process_group(GPid pid, int signal_number) noexcept {
    if (pid <= 0) return;
    if (::kill(-static_cast<pid_t>(pid), signal_number) != 0 && errno == ESRCH) {
        static_cast<void>(::kill(static_cast<pid_t>(pid), signal_number));
    }
}

void establish_process_group(gpointer) {
    static_cast<void>(::setpgid(0, 0));
}

class ChildReaper {
public:
    ChildReaper()
        : worker_([this] { run(); }) {
        worker_.detach();
    }

    void adopt(GPid pid) {
        if (pid <= 0) return;
        {
            std::lock_guard lock(mutex_);
            children_.push_back(pid);
        }
        condition_.notify_one();
    }

private:
    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            condition_.wait(lock, [this] { return !children_.empty(); });
            lock.unlock();

            bool pending = false;
            {
                std::lock_guard children_lock(mutex_);
                auto iterator = children_.begin();
                while (iterator != children_.end()) {
                    int status = 0;
                    const pid_t waited = ::waitpid(*iterator, &status, WNOHANG);
                    if (waited == *iterator || (waited < 0 && errno == ECHILD)) {
                        g_spawn_close_pid(*iterator);
                        iterator = children_.erase(iterator);
                        continue;
                    }
                    pending = true;
                    ++iterator;
                }
            }

            if (pending) std::this_thread::sleep_for(std::chrono::milliseconds(25));
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<GPid> children_;
    std::thread worker_;
};

ChildReaper& child_reaper() {
    static ChildReaper* reaper = new ChildReaper();
    return *reaper;
}

} // namespace

PowerMenuProcess::~PowerMenuProcess() {
    close();

    if (child_watch_id_ != 0) {
        g_source_remove(child_watch_id_);
        child_watch_id_ = 0;
    }
    if (control_watch_id_ != 0) {
        g_source_remove(control_watch_id_);
        control_watch_id_ = 0;
    }
    if (startup_timeout_id_ != 0) {
        g_source_remove(startup_timeout_id_);
        startup_timeout_id_ = 0;
    }
    if (shutdown_timeout_id_ != 0) {
        g_source_remove(shutdown_timeout_id_);
        shutdown_timeout_id_ = 0;
    }

    if (child_pid_ != 0) {
        const GPid child_pid = child_pid_;
        int status = 0;
        bool reaped = false;
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(kCloseGraceMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t waited = ::waitpid(child_pid, &status, WNOHANG);
            if (waited == child_pid || (waited < 0 && errno == ECHILD)) {
                reaped = true;
                break;
            }
            if (waited < 0 && errno != EINTR) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!reaped) {
            signal_process_group(child_pid, SIGTERM);
            deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kTerminateGraceMs);
            while (std::chrono::steady_clock::now() < deadline) {
                const pid_t waited = ::waitpid(child_pid, &status, WNOHANG);
                if (waited == child_pid || (waited < 0 && errno == ECHILD)) {
                    reaped = true;
                    break;
                }
                if (waited < 0 && errno != EINTR) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (!reaped) {
            signal_process_group(child_pid, SIGKILL);
            deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(kTerminateGraceMs);
            while (std::chrono::steady_clock::now() < deadline) {
                const pid_t waited = ::waitpid(child_pid, &status, WNOHANG);
                if (waited == child_pid || (waited < 0 && errno == ECHILD)) {
                    reaped = true;
                    break;
                }
                if (waited < 0 && errno != EINTR) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        if (reaped) {
            g_spawn_close_pid(child_pid);
        } else {
            child_reaper().adopt(child_pid);
        }
        if (control_fd_ >= 0) {
            ::close(control_fd_);
            control_fd_ = -1;
        }
        child_pid_ = 0;
    }
}

void PowerMenuProcess::toggle(
    int monitor_index,
    double normalized_origin_x,
    double normalized_origin_y
) {
    if (running()) {
        request_close();
        return;
    }
    static_cast<void>(launch(monitor_index, normalized_origin_x, normalized_origin_y));
}

void PowerMenuProcess::close() noexcept {
    if (!running()) return;
    request_close();
}

bool PowerMenuProcess::running() const noexcept {
    return child_pid_ != 0;
}

bool PowerMenuProcess::launch(
    int monitor_index,
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

    const std::string monitor = std::to_string(std::max(monitor_index, 0));
    std::array<gchar*, 8> arguments{
        const_cast<gchar*>(helper.c_str()),
        const_cast<gchar*>("--monitor-index"),
        const_cast<gchar*>(monitor.c_str()),
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
        &establish_process_group,
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
    terminate_sent_ = false;
    ready_ = false;
    control_buffer_.clear();
    static_cast<void>(::setpgid(child_pid_, child_pid_));
    control_fd_ = control_sockets[0];
    control_watch_id_ = g_unix_fd_add(
        control_fd_,
        static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
        &PowerMenuProcess::control_read_callback,
        this
    );
    startup_timeout_id_ = g_timeout_add(
        kStartupWatchdogMs,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<PowerMenuProcess*>(data);
            if (self == nullptr) return G_SOURCE_REMOVE;
            self->startup_timeout_id_ = 0;
            if (self->child_pid_ != 0 && !self->ready_) {
                std::cerr << "[PowerMenuProcess] helper readiness watchdog expired\n";
                self->request_close();
            }
            return G_SOURCE_REMOVE;
        },
        this
    );
    child_watch_id_ = g_child_watch_add(
        child_pid_,
        &PowerMenuProcess::child_watch_callback,
        this
    );

    std::cerr << "[PowerMenuProcess] helper started: pid=" << child_pid_
              << " monitor=" << monitor
              << " origin=" << origin_x << ',' << origin_y << '\n';
    return true;
}

void PowerMenuProcess::request_close() noexcept {
    if (child_pid_ == 0) return;

    if (startup_timeout_id_ != 0) {
        g_source_remove(startup_timeout_id_);
        startup_timeout_id_ = 0;
    }

    if (control_fd_ >= 0) {
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

    if (control_watch_id_ != 0) {
        g_source_remove(control_watch_id_);
        control_watch_id_ = 0;
    }

    if (shutdown_timeout_id_ == 0) {
        shutdown_timeout_id_ = g_timeout_add(
            kCloseGraceMs,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<PowerMenuProcess*>(data);
                if (self == nullptr) return G_SOURCE_REMOVE;
                self->shutdown_timeout_id_ = 0;
                self->escalate_shutdown();
                return G_SOURCE_REMOVE;
            },
            this
        );
    }
}

void PowerMenuProcess::escalate_shutdown() noexcept {
    if (child_pid_ == 0) return;
    if (!terminate_sent_) {
        signal_process_group(child_pid_, SIGTERM);
        terminate_sent_ = true;
        shutdown_timeout_id_ = g_timeout_add(
            kTerminateGraceMs,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<PowerMenuProcess*>(data);
                if (self == nullptr) return G_SOURCE_REMOVE;
                self->shutdown_timeout_id_ = 0;
                self->escalate_shutdown();
                return G_SOURCE_REMOVE;
            },
            this
        );
        return;
    }
    signal_process_group(child_pid_, SIGKILL);
}

void PowerMenuProcess::reap_child(int status) noexcept {
    if (control_watch_id_ != 0) {
        g_source_remove(control_watch_id_);
        control_watch_id_ = 0;
    }
    if (startup_timeout_id_ != 0) {
        g_source_remove(startup_timeout_id_);
        startup_timeout_id_ = 0;
    }
    if (control_fd_ >= 0) {
        ::close(control_fd_);
        control_fd_ = -1;
    }

    const GPid completed_pid = child_pid_;
    child_pid_ = 0;
    terminate_sent_ = false;
    ready_ = false;
    control_buffer_.clear();
    if (shutdown_timeout_id_ != 0) {
        g_source_remove(shutdown_timeout_id_);
        shutdown_timeout_id_ = 0;
    }
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
    if (!helper_override_.empty()) return helper_override_;
    const std::string executable = current_executable_path();
    if (executable.empty()) return {};
    return (
        std::filesystem::path(executable).parent_path() /
        "realmheart-power-menu-renderer"
    ).string();
}

gboolean PowerMenuProcess::control_read_callback(
    gint fd,
    GIOCondition condition,
    gpointer data
) {
    auto* self = static_cast<PowerMenuProcess*>(data);
    if (self == nullptr || self->control_fd_ != fd) return G_SOURCE_REMOVE;

    std::array<char, 128> buffer{};
    bool eof = false;
    for (;;) {
        const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (count > 0) {
            self->control_buffer_.append(
                buffer.data(),
                static_cast<std::size_t>(count)
            );
            constexpr std::size_t kMaximumControlBuffer = 4096;
            if (self->control_buffer_.size() > kMaximumControlBuffer) {
                self->control_buffer_.erase(
                    0,
                    self->control_buffer_.size() - kMaximumControlBuffer
                );
            }
            if (self->control_buffer_.find("ready\n") != std::string::npos ||
                self->control_buffer_ == "ready") {
                self->ready_ = true;
                if (self->startup_timeout_id_ != 0) {
                    g_source_remove(self->startup_timeout_id_);
                    self->startup_timeout_id_ = 0;
                }
            }
            if (count < static_cast<ssize_t>(buffer.size())) break;
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        eof = true;
        break;
    }

    if (eof || (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
        self->control_watch_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
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
