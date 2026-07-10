#pragma once

#include "core/Command.hpp"

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
    bool start_inhibitor();
    void stop_inhibitor();

    pid_t child_pid_ = -1;
};

} // namespace realmheart::services
