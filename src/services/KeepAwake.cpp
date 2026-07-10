#include "services/KeepAwake.hpp"

#include "core/Command.hpp"

#include <chrono>
#include <csignal>
#include <spawn.h>
#include <thread>
#include <sys/wait.h>

extern char** environ;

namespace realmheart::services {

KeepAwake::~KeepAwake() {
    stop_inhibitor();
}

bool KeepAwake::active(const realmheart::core::CommandOptions& options) const {
    if (!realmheart::core::command_exists("systemd-inhibit")) return false;
    const auto result = realmheart::core::run_capture(
        {"systemd-inhibit", "--list", "--no-legend", "--no-pager"},
        options
    );
    return result.succeeded()
        && !result.truncated
        && result.output.find("Realmheart") != std::string::npos
        && result.output.find("idle") != std::string::npos;
}

bool KeepAwake::set_enabled(bool enabled, const realmheart::core::CommandOptions& options) {
    if (enabled == active(options)) return true;

    if (enabled) {
        if (!start_inhibitor()) return false;
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (active(options)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        stop_inhibitor();
        return false;
    }

    stop_inhibitor();
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (!active(options)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

bool KeepAwake::start_inhibitor() {
    if (child_pid_ > 0 || !realmheart::core::command_exists("systemd-inhibit")) return child_pid_ > 0;

    char executable[] = "systemd-inhibit";
    char what[] = "--what=idle";
    char who[] = "--who=Realmheart";
    char why[] = "--why=Keep Awake toggle";
    char mode[] = "--mode=block";
    char sleep_command[] = "sleep";
    char duration[] = "infinity";
    char* arguments[] = {
        executable,
        what,
        who,
        why,
        mode,
        sleep_command,
        duration,
        nullptr,
    };

    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0) return false;
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

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
    return true;
}

void KeepAwake::stop_inhibitor() {
    if (child_pid_ <= 0) return;

    ::kill(-child_pid_, SIGTERM);
    for (int attempt = 0; attempt < 20; ++attempt) {
        const pid_t waited = ::waitpid(child_pid_, nullptr, WNOHANG);
        if (waited == child_pid_ || (waited < 0 && errno == ECHILD)) {
            child_pid_ = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    ::kill(-child_pid_, SIGKILL);
    static_cast<void>(::waitpid(child_pid_, nullptr, 0));
    child_pid_ = -1;
}

} // namespace realmheart::services
