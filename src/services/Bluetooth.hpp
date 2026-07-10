#pragma once

#include "core/Command.hpp"

#include <optional>
#include <string>

namespace realmheart::services {

struct BluetoothState {
    bool powered = false;
};

struct BluetoothMutationResult {
    bool success = false;
    BluetoothState state;
    std::string error;
};

class Bluetooth {
public:
    static std::optional<BluetoothState> read(const realmheart::core::CommandOptions& options = {});
    static BluetoothMutationResult set_powered(
        bool powered,
        const realmheart::core::CommandOptions& options = {}
    );
};

} // namespace realmheart::services
