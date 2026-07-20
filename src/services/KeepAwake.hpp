#pragma once

#include "core/Command.hpp"

#include <mutex>
#include <sys/types.h>

namespace realmheart::services {

class KeepAwake {
public:
    KeepAwake() = default;
    ~KeepAwake();

    KeepAwake(const KeepAwake&) = delete;
    KeepAwake& operator=(const KeepAwake&) = delete;

    bool active(const realmheart::core::CommandOptions& options = {}) const;
    bool set_enabled(bool enabled, const realmheart::core::CommandOptions& options = {});

private:
    bool active_locked() const;
    bool start_inhibitor_locked();
    void stop_inhibitor_locked();

    mutable std::mutex mutex_;
    mutable pid_t child_pid_ = -1;
};

} // namespace realmheart::services
