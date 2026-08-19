#pragma once

#include "core/Command.hpp"
#include "services/LockRendererProcess.hpp"
#include "services/LockSessionProvider.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace realmheart::services {

// Interface for executing shell commands, allowing for mocking in tests
class ICommandExecutor {
public:
    virtual ~ICommandExecutor() = default;
    virtual bool run_background(const std::vector<std::string>& argv) = 0;
    virtual bool run_capture_succeeded(const std::vector<std::string>& argv) = 0;
};

// Production implementation that uses real system calls
class SystemCommandExecutor : public ICommandExecutor {
public:
    bool run_background(const std::vector<std::string>& argv) override {
        return ::realmheart::core::run_background(argv);
    }
    bool run_capture_succeeded(const std::vector<std::string>& argv) override {
        return ::realmheart::core::run_capture(argv).succeeded();
    }
};

class SessionManager {
public:
    explicit SessionManager(std::unique_ptr<ICommandExecutor> executor = std::make_unique<SystemCommandExecutor>());

    // Lock the session using the Prophecy lock screen renderer.
    // Falls back to hyprlock if the renderer fails to spawn or acquire.
    bool lock();
    bool suspend();
    bool logout();
    bool reboot();
    bool power_off();

    // Lock state is now managed by an explicit enum, not pgrep-based checks.
    enum class LockState { Unlocked, Locking, Locked, Authenticating, Unlocking };
    LockState lock_state() const { return lock_state_.load(); }
    bool is_locked() const { return lock_state_.load() == LockState::Locked || lock_state_.load() == LockState::Authenticating; }

private:
    std::unique_ptr<ICommandExecutor> executor_;
    std::unique_ptr<LockSessionProvider> session_provider_;
    std::unique_ptr<LockRendererProcess> renderer_process_;
    std::atomic<LockState> lock_state_{LockState::Unlocked};
};

} // namespace realmheart::services
