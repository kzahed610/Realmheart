#pragma once

#include "core/Command.hpp"
#include <chrono>
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
    
    bool lock();
    bool suspend();
    bool logout();
    bool reboot();
    bool power_off();
    bool is_locked() const;

private:
    std::unique_ptr<ICommandExecutor> executor_;
};

} // namespace realmheart::services
