#pragma once

#include "core/Command.hpp"

#include <optional>
#include <string>

namespace realmheart::services {

struct NightLightState {
    bool enabled = false;
    int temperature = 4000;
};

struct NightLightMutationResult {
    bool success = false;
    NightLightState state;
    std::string error;
};

class NightLight {
public:
    static constexpr int kMinimumTemperature = 2500;
    static constexpr int kMaximumTemperature = 6000;
    static constexpr int kDefaultTemperature = 4000;

    static std::optional<NightLightState> read(
        const realmheart::core::CommandOptions& options = {}
    );
    static NightLightMutationResult set_enabled(
        bool enabled,
        const realmheart::core::CommandOptions& options = {}
    );
    static NightLightMutationResult set_enabled(
        bool enabled,
        int temperature,
        const realmheart::core::CommandOptions& options = {}
    );
    static NightLightMutationResult set_temperature(
        int temperature,
        const realmheart::core::CommandOptions& options = {}
    );

    [[nodiscard]] static int strength_to_temperature(int strength_percent);
    [[nodiscard]] static int temperature_to_strength(int temperature);
};

} // namespace realmheart::services
