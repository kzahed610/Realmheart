#include "services/SessionManager.hpp"

namespace realmheart::services {

SessionManager::SessionManager(std::unique_ptr<ICommandExecutor> executor) 
    : executor_(std::move(executor)) {}

bool SessionManager::lock() {
    return executor_->run_background({"hyprlock"});
}

bool SessionManager::suspend() {
    return executor_->run_capture_succeeded({"systemctl", "suspend"});
}

bool SessionManager::logout() {
    return executor_->run_capture_succeeded({"hyprctl", "dispatch", "exit"});
}

bool SessionManager::reboot() {
    return executor_->run_capture_succeeded({"systemctl", "reboot"});
}

bool SessionManager::power_off() {
    return executor_->run_capture_succeeded({"systemctl", "poweroff"});
}

bool SessionManager::is_locked() const {
    return executor_->run_capture_succeeded({"pgrep", "-x", "hyprlock"});
}

} // namespace realmheart::services
