#include "services/KeepAwake.hpp"

#include "core/Command.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <spawn.h>
#include <thread>
#include <sys/wait.h>

extern char** environ;

namespace realmheart::services {

KeepAwake::~KeepAwake() {
    std::lock_guard lock(mutex_);
    stop_inhibitor_locked();
}

bool KeepAwake::active(const realmheart::core::CommandOptions&) const {
    std::lock_guard lock(mutex_);
    return active_locked();
}

bool KeepAwake::active_locked() const {
    if (child_pid_ <= 0) return false;

    int status = 0;
    const pid_t waited = ::waitpid(child_pid_, &status, WNOHANG);
    if (waited == child_pid_ || (waited < 0 && errno == ECHILD)) {
        child_pid_ = -1;
        return false;
    }
    if (waited < 0 && errno != EINTR) return false;

    errno = 0;
    return ::kill(child_pid_, 0) == 0 || errno == EPERM;
}

bool KeepAwake::set_enabled(bool enabled, const realmheart::core::CommandOptions&) {
    std::lock_guard lock(mutex_);
    if (enabled == active_locked()) return true;
    if (enabled) return start_inhibitor_locked() && active_locked();
    stop_inhibitor_locked();
    return !active_locked();
}

bool KeepAwake::start_inhibitor_locked() {
    if (child_pid_ > 0) return active_locked();
    if (!realmheart::core::command_exists("systemd-inhibit")) return false;

    char executable[] = "systemd-inhibit";
    char what[] = "--what=idle";
    char who[] = "--who=Realmheart";
    char why[] = "--why=Keep Awake toggle";
    char mode[] = "--mode=block";
    char sleep_command[] = "sleep";
    char duration[] = "infinity";
    char* arguments[] = {
        executable, what, who, why, mode, sleep_command, duration, nullptr,
    };

    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0) return false;
    const short flags = POSIX_SPAWN_SETPGROUP;
    if (posix_spawnattr_setflags(&attributes, flags) != 0 ||
        posix_spawnattr_setpgroup(&attributes, 0) != 0) {
        posix_spawnattr_destroy(&attributes);
        return false;
    }

    pid_t child = -1;
    const int result = posix_spawnp(
        &child,
        executable,
        nullptr,
        &attributes,
        arguments,
        environ
    );
    posix_spawnattr_destroy(&attributes);
    if (result != 0) return false;

    child_pid_ = child;
    // Detect an immediate exec/runtime failure without repeatedly invoking
    // systemd-inhibit --list.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int status = 0;
    if (::waitpid(child_pid_, &status, WNOHANG) == child_pid_) {
        child_pid_ = -1;
        return false;
    }
    return true;
}

void KeepAwake::stop_inhibitor_locked() {
    if (child_pid_ <= 0) return;

    static_cast<void>(::kill(-child_pid_, SIGTERM));
    for (int attempt = 0; attempt < 10; ++attempt) {
        const pid_t waited = ::waitpid(child_pid_, nullptr, WNOHANG);
        if (waited == child_pid_ || (waited < 0 && errno == ECHILD)) {
            child_pid_ = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    static_cast<void>(::kill(-child_pid_, SIGKILL));
    while (::waitpid(child_pid_, nullptr, 0) < 0 && errno == EINTR) {
    }
    child_pid_ = -1;
}

} // namespace realmheart::services
