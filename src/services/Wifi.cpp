#include "services/Wifi.hpp"

#include "core/Command.hpp"

#include <sstream>
#include <string_view>

namespace realmheart::services {
namespace {

bool usable_output(const realmheart::core::CommandResult& result) {
    return result.succeeded() && !result.truncated && !result.output.empty();
}

std::string active_ssid(const realmheart::core::CommandOptions& options) {
    const auto scan = realmheart::core::run_capture(
        {"nmcli", "-t", "-f", "ACTIVE,SSID", "dev", "wifi"},
        options
    );
    if (!usable_output(scan)) return {};

    std::stringstream lines(scan.output);
    std::string line;
    while (std::getline(lines, line)) {
        constexpr std::string_view prefix = "yes:";
        if (line.starts_with(prefix) && line.size() > prefix.size()) {
            return realmheart::core::sanitize_command_detail(line.substr(prefix.size()), 96);
        }
    }
    return {};
}

} // namespace

std::optional<WifiState> Wifi::read(const realmheart::core::CommandOptions& options) {
    if (!realmheart::core::command_exists("nmcli")) return std::nullopt;

    const auto radio = realmheart::core::run_capture({"nmcli", "radio", "wifi"}, options);
    if (!usable_output(radio)) return std::nullopt;

    WifiState state;
    if (radio.output == "enabled") {
        state.enabled = true;
        state.ssid = active_ssid(options);
    } else if (radio.output != "disabled") {
        return std::nullopt;
    }
    return state;
}

WifiMutationResult Wifi::set_enabled(
    bool enabled,
    const realmheart::core::CommandOptions& options
) {
    WifiMutationResult mutation;
    if (!realmheart::core::command_exists("nmcli")) {
        mutation.error = "nmcli not found";
        return mutation;
    }

    const auto write = realmheart::core::run_capture(
        {"nmcli", "radio", "wifi", enabled ? "on" : "off"},
        options
    );
    if (!write.succeeded()) {
        mutation.error = realmheart::core::command_failure_detail(write, "nmcli radio wifi failed");
        return mutation;
    }

    const auto readback = read(options);
    if (!readback) {
        mutation.error = "WiFi state unavailable after mutation";
        return mutation;
    }

    mutation.state = *readback;
    mutation.success = mutation.state.enabled == enabled;
    if (!mutation.success) mutation.error = "WiFi readback did not match requested state";
    return mutation;
}

} // namespace realmheart::services
