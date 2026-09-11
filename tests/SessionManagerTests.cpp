#include "services/SessionManager.hpp"
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <memory>
#include <optional>
#include <sys/types.h>

class MockCommandExecutor : public realmheart::services::ICommandExecutor {
public:
    std::vector<std::vector<std::string>> background_calls;
    std::vector<std::vector<std::string>> capture_calls;
    bool next_capture_result = true;
    bool next_background_result = true;
    bool bounded_capture_used = false;
    std::chrono::milliseconds bounded_capture_deadline{};
    std::vector<std::vector<std::string>> tracked_background_calls;
    std::optional<pid_t> next_tracked_pid = 4242;

    bool run_background(const std::vector<std::string>& argv) override {
        background_calls.push_back(argv);
        return next_background_result;
    }
    std::optional<pid_t> run_background_tracked(
        const std::vector<std::string>& argv
    ) override {
        tracked_background_calls.push_back(argv);
        return next_tracked_pid;
    }
    bool run_capture_succeeded(const std::vector<std::string>& argv) override {
        capture_calls.push_back(argv);
        return next_capture_result;
    }
    bool run_capture_succeeded_bounded(
        const std::vector<std::string>& argv,
        std::chrono::milliseconds deadline
    ) override {
        bounded_capture_used = true;
        bounded_capture_deadline = deadline;
        return run_capture_succeeded(argv);
    }
};

void test_fallback_lock_triggers_hyprlock() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    bool result = session.fallback_lock();

    if (!result) { std::cerr << "Lock failed\n"; exit(1); }
    if (mock_ptr->background_calls.size() != 1) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->background_calls[0] != std::vector<std::string>{"/usr/bin/hyprlock"}) { std::cerr << "Wrong trusted command\n"; exit(1); }
    std::cout << "test_fallback_lock_triggers_hyprlock PASSED\n";
}

void test_suspend_triggers_systemd_suspend() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    bool result = session.suspend();

    if (!result) { std::cerr << "Suspend failed\n"; exit(1); }
    if (mock_ptr->capture_calls.size() != 1) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->capture_calls[0] != std::vector<std::string>{"/usr/bin/systemctl", "suspend"}) { std::cerr << "Wrong trusted command\n"; exit(1); }
    std::cout << "test_suspend_triggers_systemd_suspend PASSED\n";
}

void test_tracked_fallback_preserves_child_identity() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    const auto pid = session.fallback_lock_tracked();
    assert(pid && *pid == 4242);
    assert(mock_ptr->tracked_background_calls ==
           std::vector<std::vector<std::string>>{{"/usr/bin/hyprlock"}});
    std::cout << "test_tracked_fallback_preserves_child_identity PASSED\n";
}

void test_session_action_reports_post_exec_failure() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    mock_ptr->next_capture_result = false;
    realmheart::services::SessionManager session(std::move(mock));

    if (session.suspend()) { std::cerr << "Failed suspend must be reported\n"; exit(1); }
    if (mock_ptr->capture_calls != std::vector<std::vector<std::string>>{{"/usr/bin/systemctl", "suspend"}}) {
        std::cerr << "Failed suspend must use the finite command path\n";
        exit(1);
    }
    std::cout << "test_session_action_reports_post_exec_failure PASSED\n";
}

void test_emergency_lock_is_best_effort_and_bounded() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    mock_ptr->next_capture_result = false;
    realmheart::services::SessionManager session(std::move(mock));

    session.request_emergency_lock();
    if (!mock_ptr->bounded_capture_used ||
        mock_ptr->bounded_capture_deadline != std::chrono::seconds(1)) {
        std::cerr << "Emergency lock must use a bounded best-effort request\n";
        exit(1);
    }
    if (mock_ptr->capture_calls != std::vector<std::vector<std::string>>{
            {"/usr/bin/loginctl", "lock-session"}
        }) {
        std::cerr << "Emergency lock must use the trusted loginctl path\n";
        exit(1);
    }
    std::cout << "test_emergency_lock_is_best_effort_and_bounded PASSED\n";
}

void test_logout_triggers_hyprland_exit() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    bool result = session.logout();

    if (!result) { std::cerr << "Logout failed\n"; exit(1); }
    if (mock_ptr->capture_calls.size() != 1) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->capture_calls[0] != std::vector<std::string>{"/usr/bin/hyprctl", "dispatch", "exit"}) { std::cerr << "Wrong trusted command\n"; exit(1); }
    std::cout << "test_logout_triggers_hyprland_exit PASSED\n";
}

void test_reboot_triggers_systemd_reboot() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    bool result = session.reboot();

    if (!result) { std::cerr << "Reboot failed\n"; exit(1); }
    if (mock_ptr->capture_calls.size() != 1) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->capture_calls[0] != std::vector<std::string>{"/usr/bin/systemctl", "reboot"}) { std::cerr << "Wrong trusted command\n"; exit(1); }
    std::cout << "test_reboot_triggers_systemd_reboot PASSED\n";
}

void test_power_off_triggers_systemd_poweroff() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    bool result = session.power_off();

    if (!result) { std::cerr << "Power off failed\n"; exit(1); }
    if (mock_ptr->capture_calls.size() != 1) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->capture_calls[0] != std::vector<std::string>{"/usr/bin/systemctl", "poweroff"}) { std::cerr << "Wrong trusted command\n"; exit(1); }
    std::cout << "test_power_off_triggers_systemd_poweroff PASSED\n";
}

int main() {
    test_fallback_lock_triggers_hyprlock();
    test_suspend_triggers_systemd_suspend();
    test_tracked_fallback_preserves_child_identity();
    test_session_action_reports_post_exec_failure();
    test_emergency_lock_is_best_effort_and_bounded();
    test_logout_triggers_hyprland_exit();
    test_reboot_triggers_systemd_reboot();
    test_power_off_triggers_systemd_poweroff();

    std::cout << "All SessionManager tests PASSED (MOCKED)\n";
    return 0;
}
