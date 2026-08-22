#include "services/SessionManager.hpp"

namespace realmheart::services {

SessionManager::SessionManager(std::unique_ptr<ICommandExecutor> executor) 
    : executor_(std::move(executor)) {}

bool SessionManager::lock() {
    return executor_->run_background({"hyprlock"});
}

bool SessionManager::suspend() {
    return executor_->run_background({"systemctl", "suspend"});
}

bool SessionManager::logout() {
    return executor_->run_background({"hyprctl", "dispatch", "exit"});
}

bool SessionManager::reboot() {
    return executor_->run_background({"systemctl", "reboot"});
}

bool SessionManager::power_off() {
    return executor_->run_background({"systemctl", "poweroff"});
}

bool SessionManager::is_locked() const {
    return executor_->run_capture_succeeded({"pgrep", "hyprlock"});
}

bool SessionManager::enable_lockscreen_blur() const {
    return executor_->run_background({
        "hyprctl", "eval", "layerrule = \"blur, realmheart-broken_seal\""
    });
}

bool SessionManager::disable_lockscreen_blur() const {
    // Sweep ALL rule state for the seal namespace (not just blur): a shell
    // killed mid-lock (TTY escape, crash) leaves its rules behind, and the
    // next lock would then start with stale blur/noanim already armed —
    // which is exactly the "blur is always there" symptom.
    const bool unset_all = executor_->run_background({
        "hyprctl", "eval", "layerrule = \"unset, realmheart-broken_seal\""
    });
    executor_->run_background({
        "hyprctl", "eval",
        "layerrule = \"unset, realmheart-broken_seal, blur\""
    });
    return unset_all;
}

} // namespace realmheart::services
