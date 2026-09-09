#include "services/SessionManager.hpp"

namespace realmheart::services {
namespace {

constexpr const char* kHyprlock = "/usr/bin/hyprlock";
constexpr const char* kSystemctl = "/usr/bin/systemctl";
constexpr const char* kHyprctl = "/usr/bin/hyprctl";
constexpr const char* kPgrep = "/usr/bin/pgrep";

} // namespace

SessionManager::SessionManager(std::unique_ptr<ICommandExecutor> executor) 
    : executor_(std::move(executor)) {}

bool SessionManager::lock() {
    return executor_->run_background({kHyprlock});
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

bool SessionManager::is_locked() const {
    return executor_->run_capture_succeeded_bounded(
        {kPgrep, "-x", "hyprlock"},
        std::chrono::milliseconds(250)
    );
}

} // namespace realmheart::services
