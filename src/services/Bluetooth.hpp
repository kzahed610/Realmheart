#pragma once

#include "core/Command.hpp"

#include <optional>
#include <string>
#include <vector>

namespace realmheart::services {

struct BluetoothState {
    bool powered = false;
};

struct BluetoothMutationResult {
    bool success = false;
    BluetoothState state;
    std::string error;
};

struct BluetoothDevice {
    std::string address;
    std::string name;
    bool paired = false;
    bool trusted = false;
    bool connected = false;
};

struct BluetoothDeviceMutationResult {
    bool success = false;
    std::optional<BluetoothDevice> device;
    std::string error;
};

class Bluetooth {
public:
    static std::optional<BluetoothState> read(const realmheart::core::CommandOptions& options = {});
    static BluetoothMutationResult set_powered(
        bool powered,
        const realmheart::core::CommandOptions& options = {}
    );
    static std::vector<BluetoothDevice> devices(
        bool scan_for_new_devices = true,
        const realmheart::core::CommandOptions& options = {}
    );
    static BluetoothDeviceMutationResult connect(
        const std::string& address,
        const realmheart::core::CommandOptions& options = {}
    );
    static BluetoothDeviceMutationResult disconnect(
        const std::string& address,
        const realmheart::core::CommandOptions& options = {}
    );
    static BluetoothDeviceMutationResult forget(
        const std::string& address,
        const realmheart::core::CommandOptions& options = {}
    );
};

} // namespace realmheart::services
