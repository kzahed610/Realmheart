#include "services/SessionManager.hpp"

namespace realmheart::services {

SessionManager::SessionManager(std::unique_ptr<ICommandExecutor> executor) 
    : executor_(std::move(executor)) {}

bool SessionManager::lock() {
    return executor_->run_background({"hyprlock"});
}

bool SessionManager::logout_menu() {
    return executor_->run_background({"wlogout"});
}

bool SessionManager::is_locked() const {
    return executor_->run_capture_succeeded({"pgrep", "hyprlock"});
}

} // namespace realmheart::services
