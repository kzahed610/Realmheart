#include "services/Bluetooth.hpp"

#include "core/Command.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <vector>

namespace realmheart::services {
namespace {

std::optional<BluetoothDevice> read_device(
    const std::string& address,
    const realmheart::core::CommandOptions& options
) {
    const auto info = realmheart::core::run_capture(
        {"bluetoothctl", "info", address}, options
    );
    if (!info.succeeded() || info.truncated || info.output.empty()) return std::nullopt;

    BluetoothDevice device;
    device.address = address;
    std::stringstream lines(info.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto key = realmheart::core::trim(line.substr(0, colon));
        const auto value = realmheart::core::trim(line.substr(colon + 1));
        if (key == "Name" || (key == "Alias" && device.name.empty())) {
            device.name = realmheart::core::sanitize_command_detail(value, 96);
        } else if (key == "Paired") {
            device.paired = value == "yes";
        } else if (key == "Trusted") {
            device.trusted = value == "yes";
        } else if (key == "Connected") {
            device.connected = value == "yes";
        }
    }
    if (device.name.empty()) device.name = address;
    return device;
}

BluetoothDeviceMutationResult command_failure(
    const realmheart::core::CommandResult& result,
    std::string_view fallback
) {
    BluetoothDeviceMutationResult mutation;
    mutation.error = realmheart::core::command_failure_detail(result, fallback);
    return mutation;
}

bool valid_address(std::string_view address) {
    if (address.size() != 17) return false;
    for (std::size_t index = 0; index < address.size(); ++index) {
        const char character = address[index];
        if ((index + 1) % 3 == 0) {
            if (character != ':') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(character))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<BluetoothState> Bluetooth::read(const realmheart::core::CommandOptions& options) {
    if (!realmheart::core::command_exists("bluetoothctl")) return std::nullopt;

    const auto show = realmheart::core::run_capture({"bluetoothctl", "show"}, options);
    if (!show.succeeded() || show.truncated || show.output.empty()) return std::nullopt;

    BluetoothState state;
    if (show.output.find("Powered: yes") != std::string::npos) {
        state.powered = true;
    } else if (show.output.find("Powered: no") == std::string::npos) {
        return std::nullopt;
    }
    return state;
}

BluetoothMutationResult Bluetooth::set_powered(
    bool powered,
    const realmheart::core::CommandOptions& options
) {
    BluetoothMutationResult mutation;
    if (!realmheart::core::command_exists("bluetoothctl")) {
        mutation.error = "bluetoothctl not found";
        return mutation;
    }

    const auto write = realmheart::core::run_capture(
        {"bluetoothctl", "power", powered ? "on" : "off"},
        options
    );
    if (!write.succeeded()) {
        mutation.error = realmheart::core::command_failure_detail(write, "bluetoothctl power failed");
        return mutation;
    }

    const auto readback = read(options);
    if (!readback) {
        mutation.error = "Bluetooth state unavailable after mutation";
        return mutation;
    }

    mutation.state = *readback;
    mutation.success = mutation.state.powered == powered;
    if (!mutation.success) mutation.error = "Bluetooth readback did not match requested state";
    return mutation;
}

std::vector<BluetoothDevice> Bluetooth::devices(
    bool scan_for_new_devices,
    const realmheart::core::CommandOptions& options
) {
    if (!realmheart::core::command_exists("bluetoothctl")) return {};
    const auto state = read(options);
    if (!state || !state->powered) return {};

    if (scan_for_new_devices) {
        static_cast<void>(realmheart::core::run_capture(
            {"bluetoothctl", "--timeout", "5", "scan", "on"}, options
        ));
    }

    const auto listing = realmheart::core::run_capture({"bluetoothctl", "devices"}, options);
    if (!listing.succeeded() || listing.truncated) return {};

    std::vector<BluetoothDevice> result;
    std::stringstream lines(listing.output);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.starts_with("Device ") || line.size() < 24) continue;
        const std::string address = line.substr(7, 17);
        if (!valid_address(address)) continue;
        auto device = read_device(address, options);
        if (!device) {
            BluetoothDevice fallback;
            fallback.address = address;
            fallback.name = realmheart::core::sanitize_command_detail(line.substr(25), 96);
            if (fallback.name.empty()) fallback.name = address;
            result.push_back(std::move(fallback));
        } else {
            result.push_back(std::move(*device));
        }
    }

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.connected != right.connected) return left.connected > right.connected;
        if (left.paired != right.paired) return left.paired > right.paired;
        return left.name < right.name;
    });
    return result;
}

BluetoothDeviceMutationResult Bluetooth::connect(
    const std::string& address,
    const realmheart::core::CommandOptions& options
) {
    BluetoothDeviceMutationResult mutation;
    if (!realmheart::core::command_exists("bluetoothctl")) {
        mutation.error = "bluetoothctl not found";
        return mutation;
    }
    if (!valid_address(address)) {
        mutation.error = "Invalid Bluetooth address";
        return mutation;
    }

    auto device = read_device(address, options);
    if (!device) {
        mutation.error = "Bluetooth device is no longer available";
        return mutation;
    }
    if (!device->paired) {
        const auto pair = realmheart::core::run_capture(
            {"bluetoothctl", "--timeout", "15", "pair", address}, options
        );
        if (!pair.succeeded()) return command_failure(pair, "Bluetooth pairing failed");
    }
    if (!device->trusted) {
        static_cast<void>(realmheart::core::run_capture(
            {"bluetoothctl", "trust", address}, options
        ));
    }

    const auto connect = realmheart::core::run_capture(
        {"bluetoothctl", "connect", address}, options
    );
    if (!connect.succeeded()) return command_failure(connect, "Bluetooth connection failed");

    mutation.device = read_device(address, options);
    mutation.success = mutation.device.has_value() && mutation.device->connected;
    if (!mutation.success) mutation.error = "Bluetooth device did not report connected";
    return mutation;
}

BluetoothDeviceMutationResult Bluetooth::disconnect(
    const std::string& address,
    const realmheart::core::CommandOptions& options
) {
    BluetoothDeviceMutationResult mutation;
    if (!realmheart::core::command_exists("bluetoothctl")) {
        mutation.error = "bluetoothctl not found";
        return mutation;
    }
    if (!valid_address(address)) {
        mutation.error = "Invalid Bluetooth address";
        return mutation;
    }

    const auto disconnect = realmheart::core::run_capture(
        {"bluetoothctl", "disconnect", address}, options
    );
    if (!disconnect.succeeded()) {
        return command_failure(disconnect, "Bluetooth disconnect failed");
    }
    mutation.device = read_device(address, options);
    mutation.success = mutation.device.has_value() && !mutation.device->connected;
    if (!mutation.success) mutation.error = "Bluetooth device remained connected";
    return mutation;
}

BluetoothDeviceMutationResult Bluetooth::forget(
    const std::string& address,
    const realmheart::core::CommandOptions& options
) {
    BluetoothDeviceMutationResult mutation;
    if (!realmheart::core::command_exists("bluetoothctl")) {
        mutation.error = "bluetoothctl not found";
        return mutation;
    }
    if (!valid_address(address)) {
        mutation.error = "Invalid Bluetooth address";
        return mutation;
    }

    const auto remove = realmheart::core::run_capture(
        {"bluetoothctl", "remove", address}, options
    );
    if (!remove.succeeded()) return command_failure(remove, "Removing Bluetooth device failed");
    mutation.success = true;
    return mutation;
}

} // namespace realmheart::services
