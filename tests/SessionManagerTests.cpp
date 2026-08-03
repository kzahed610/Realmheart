#include "services/SessionManager.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <memory>

class MockCommandExecutor : public realmheart::services::ICommandExecutor {
public:
    std::vector<std::vector<std::string>> background_calls;
    std::vector<std::vector<std::string>> capture_calls;
    bool next_capture_result = true;
    bool next_background_result = true;

    bool run_background(const std::vector<std::string>& argv) override {
        background_calls.push_back(argv);
        return next_background_result;
    }
    bool run_capture_succeeded(const std::vector<std::string>& argv) override {
        capture_calls.push_back(argv);
        return next_capture_result;
    }
};

void test_lock_triggers_hyprlock() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    bool result = session.lock();

    if (!result) { std::cerr << "Lock failed\n"; exit(1); }
    if (mock_ptr->background_calls.size() != 1) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->background_calls[0] != std::vector<std::string>{"hyprlock"}) { std::cerr << "Wrong command\n"; exit(1); }
    std::cout << "test_lock_triggers_hyprlock PASSED\n";
}

void test_suspend_triggers_systemd_suspend() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    bool result = session.suspend();

    if (!result) { std::cerr << "Suspend failed\n"; exit(1); }
    if (mock_ptr->background_calls.size() != 1) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->background_calls[0] != std::vector<std::string>{"systemctl", "suspend"}) { std::cerr << "Wrong command\n"; exit(1); }
    std::cout << "test_suspend_triggers_systemd_suspend PASSED\n";
}

void test_logout_triggers_hyprland_exit() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    bool result = session.logout();

    if (!result) { std::cerr << "Logout failed\n"; exit(1); }
    if (mock_ptr->background_calls.size() != 1) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->background_calls[0] != std::vector<std::string>{"hyprctl", "dispatch", "exit"}) { std::cerr << "Wrong command\n"; exit(1); }
    std::cout << "test_logout_triggers_hyprland_exit PASSED\n";
}

void test_reboot_triggers_systemd_reboot() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    bool result = session.reboot();

    if (!result) { std::cerr << "Reboot failed\n"; exit(1); }
    if (mock_ptr->background_calls.size() != 1) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->background_calls[0] != std::vector<std::string>{"systemctl", "reboot"}) { std::cerr << "Wrong command\n"; exit(1); }
    std::cout << "test_reboot_triggers_systemd_reboot PASSED\n";
}

void test_power_off_triggers_systemd_poweroff() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    bool result = session.power_off();

    if (!result) { std::cerr << "Power off failed\n"; exit(1); }
    if (mock_ptr->background_calls.size() != 1) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->background_calls[0] != std::vector<std::string>{"systemctl", "poweroff"}) { std::cerr << "Wrong command\n"; exit(1); }
    std::cout << "test_power_off_triggers_systemd_poweroff PASSED\n";
}

void test_is_locked_checks_pgrep() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    mock_ptr->next_capture_result = true;
    if (!session.is_locked()) { std::cerr << "Should be locked\n"; exit(1); }

    mock_ptr->next_capture_result = false;
    if (session.is_locked()) { std::cerr << "Should be unlocked\n"; exit(1); }

    if (mock_ptr->capture_calls.size() != 2) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->capture_calls[0] != std::vector<std::string>{"pgrep", "hyprlock"}) { std::cerr << "Wrong command\n"; exit(1); }
    std::cout << "test_is_locked_checks_pgrep PASSED\n";
}

int main() {
    test_lock_triggers_hyprlock();
    test_suspend_triggers_systemd_suspend();
    test_logout_triggers_hyprland_exit();
    test_reboot_triggers_systemd_reboot();
    test_power_off_triggers_systemd_poweroff();
    test_is_locked_checks_pgrep();
    std::cout << "All SessionManager tests PASSED (MOCKED)\n";
    return 0;
}
