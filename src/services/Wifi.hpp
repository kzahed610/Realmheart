#pragma once

#include "core/Command.hpp"

#include <optional>
#include <string>
#include <vector>

namespace realmheart::services {

struct WifiState {
    bool enabled = false;
    std::string ssid;
    std::optional<int> signal_percent;
};

struct WifiMutationResult {
    bool success = false;
    WifiState state;
    std::string error;
};

struct WifiNetwork {
    std::string ssid;
    std::string bssid;
    std::string security;
    std::string connection_uuid;
    int signal_percent = 0;
    bool active = false;
    bool saved = false;

    [[nodiscard]] bool secured() const noexcept {
        return !security.empty() && security != "--" && security != "NONE";
    }
};

class Wifi {
public:
    static std::optional<WifiState> read(const realmheart::core::CommandOptions& options = {});
    static WifiMutationResult set_enabled(
        bool enabled,
        const realmheart::core::CommandOptions& options = {}
    );
    static std::vector<WifiNetwork> scan(
        bool rescan = true,
        const realmheart::core::CommandOptions& options = {}
    );
    static WifiMutationResult connect(
        const std::string& ssid,
        const std::optional<std::string>& password = std::nullopt,
        const std::string& connection_uuid = {},
        const realmheart::core::CommandOptions& options = {}
    );
    static WifiMutationResult disconnect(
        const realmheart::core::CommandOptions& options = {}
    );
    static WifiMutationResult forget(
        const std::string& ssid,
        const std::string& connection_uuid = {},
        const realmheart::core::CommandOptions& options = {}
    );
};

} // namespace realmheart::services
