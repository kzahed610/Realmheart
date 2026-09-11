#include "services/SessionManager.hpp"

namespace realmheart::services {
namespace {

constexpr const char* kHyprlock = "/usr/bin/hyprlock";
constexpr const char* kSystemctl = "/usr/bin/systemctl";
constexpr const char* kHyprctl = "/usr/bin/hyprctl";
constexpr const char* kLoginctl = "/usr/bin/loginctl";

} // namespace

SessionManager::SessionManager(std::unique_ptr<ICommandExecutor> executor) 
    : executor_(std::move(executor)) {}

bool SessionManager::fallback_lock() {
    return executor_->run_background({kHyprlock});
}

std::optional<pid_t> SessionManager::fallback_lock_tracked() {
    return executor_->run_background_tracked({kHyprlock});
}

void SessionManager::request_emergency_lock() {
    static_cast<void>(executor_->run_capture_succeeded_bounded(
        {kLoginctl, "lock-session"},
        std::chrono::seconds(1)
    ));
}

bool SessionManager::suspend() {
    return executor_->run_capture_succeeded({kSystemctl, "suspend"});
}

bool SessionManager::logout() {
    return executor_->run_capture_succeeded({kHyprctl, "dispatch", "exit"});
}

bool SessionManager::reboot() {
    return executor_->run_capture_succeeded({kSystemctl, "reboot"});
}

bool SessionManager::power_off() {
    return executor_->run_capture_succeeded({kSystemctl, "poweroff"});
}

} // namespace realmheart::services
