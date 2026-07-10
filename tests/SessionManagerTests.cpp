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

void test_logout_triggers_wlogout() {
    auto mock = std::make_unique<MockCommandExecutor>();
    auto* mock_ptr = mock.get();
    realmheart::services::SessionManager session(std::move(mock));

    bool result = session.logout_menu();

    if (!result) { std::cerr << "Logout failed\n"; exit(1); }
    if (mock_ptr->background_calls.size() != 1) { std::cerr << "Wrong call count\n"; exit(1); }
    if (mock_ptr->background_calls[0] != std::vector<std::string>{"wlogout"}) { std::cerr << "Wrong command\n"; exit(1); }
    std::cout << "test_logout_triggers_wlogout PASSED\n";
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
    test_logout_triggers_wlogout();
    test_is_locked_checks_pgrep();
    std::cout << "All SessionManager tests PASSED (MOCKED)\n";
    return 0;
}
