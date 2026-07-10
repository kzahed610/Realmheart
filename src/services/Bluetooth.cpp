#include "services/Bluetooth.hpp"

#include "core/Command.hpp"

namespace realmheart::services {

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

} // namespace realmheart::services
