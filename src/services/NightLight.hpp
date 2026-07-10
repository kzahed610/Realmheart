#pragma once

#include "core/Command.hpp"

#include <optional>
#include <string>

namespace realmheart::services {

struct NightLightState {
    bool enabled = false;
    int temperature = 6500;
};

struct NightLightMutationResult {
    bool success = false;
    NightLightState state;
    std::string error;
};

class NightLight {
public:
    static std::optional<NightLightState> read(const realmheart::core::CommandOptions& options = {});
    static NightLightMutationResult set_enabled(
        bool enabled,
        const realmheart::core::CommandOptions& options = {}
    );
};

} // namespace realmheart::services
