#pragma once

#include "core/Command.hpp"
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>
#include <functional>

namespace realmheart::services {

// Interface for executing shell commands, allowing for mocking in tests
class ICommandExecutor {
public:
    virtual ~ICommandExecutor() = default;
    virtual bool run_background(const std::vector<std::string>& argv) = 0;
    virtual std::optional<pid_t> run_background_tracked(
        const std::vector<std::string>& argv
    ) {
        static_cast<void>(argv);
        return std::nullopt;
    }
    virtual bool run_capture_succeeded(const std::vector<std::string>& argv) = 0;
    virtual bool run_capture_succeeded_bounded(
        const std::vector<std::string>& argv,
        std::chrono::milliseconds deadline
    ) {
        static_cast<void>(deadline);
        return run_capture_succeeded(argv);
    }
};

// Production implementation that uses real system calls
class SystemCommandExecutor : public ICommandExecutor {
public:
    bool run_background(const std::vector<std::string>& argv) override {
        return ::realmheart::core::run_background(argv);
    }
    std::optional<pid_t> run_background_tracked(
        const std::vector<std::string>& argv
    ) override {
        const auto process = ::realmheart::core::run_background_tracked(argv);
        if (!process) return std::nullopt;
        return process->pid;
    }
    bool run_capture_succeeded(const std::vector<std::string>& argv) override {
        return ::realmheart::core::run_capture(argv).succeeded();
    }
    bool run_capture_succeeded_bounded(
        const std::vector<std::string>& argv,
        std::chrono::milliseconds deadline
    ) override {
        ::realmheart::core::CommandOptions options;
        options.deadline = deadline;
        return ::realmheart::core::run_capture(argv, options).succeeded();
    }
};

class SessionManager {
public:
    explicit SessionManager(std::unique_ptr<ICommandExecutor> executor = std::make_unique<SystemCommandExecutor>());
    
    // Deliberate fail-closed fallback used only when native Broken Seal
    // choreography cannot be established by the persistent shell.
    bool fallback_lock();
    [[nodiscard]] std::optional<pid_t> fallback_lock_tracked();
    // Best-effort emergency request used only when fullscreen lock coverage
    // could not be verified. The result is intentionally discarded: a
    // loginctl exit status is not proof that this session is covered.
    void request_emergency_lock();
    bool suspend();
    bool logout();
    bool reboot();
    bool power_off();
private:
    std::unique_ptr<ICommandExecutor> executor_;
};

} // namespace realmheart::services
