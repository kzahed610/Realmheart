#pragma once

#include "core/Command.hpp"

#include <optional>
#include <string>

namespace realmheart::services {

struct WifiState {
    bool enabled = false;
    std::string ssid;
};

struct WifiMutationResult {
    bool success = false;
    WifiState state;
    std::string error;
};

class Wifi {
public:
    static std::optional<WifiState> read(const realmheart::core::CommandOptions& options = {});
    static WifiMutationResult set_enabled(
        bool enabled,
        const realmheart::core::CommandOptions& options = {}
    );
};

} // namespace realmheart::services
