#include "services/Wifi.hpp"

#include "core/Command.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <string_view>
#include <vector>

namespace realmheart::services {
namespace {


std::vector<std::string> split_nmcli_fields(std::string_view line) {
    std::vector<std::string> fields;
    std::string current;
    bool escaped = false;
    for (const char character : line) {
        if (escaped) {
            current.push_back(character);
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == ':') {
            fields.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    if (escaped) current.push_back('\\');
    fields.push_back(std::move(current));
    return fields;
}

std::optional<int> parse_signal(std::string_view value) {
    int signal = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), signal);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return std::clamp(signal, 0, 100);
}

bool usable_output(const realmheart::core::CommandResult& result) {
    return result.succeeded() && !result.truncated && !result.output.empty();
}

struct ActiveNetwork {
    std::string ssid;
    std::optional<int> signal_percent;
};

ActiveNetwork active_network(const realmheart::core::CommandOptions& options) {
    const auto scan = realmheart::core::run_capture(
        {"nmcli", "-t", "-f", "ACTIVE,SSID,SIGNAL", "dev", "wifi"},
        options
    );
    if (!usable_output(scan)) return {};

    std::stringstream lines(scan.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = split_nmcli_fields(line);
        if (fields.size() < 2 || fields[0] != "yes") continue;

        ActiveNetwork network;
        network.ssid = realmheart::core::sanitize_command_detail(fields[1], 96);
        if (fields.size() >= 3) network.signal_percent = parse_signal(fields[2]);
        return network;
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
        const auto active = active_network(options);
        state.ssid = active.ssid;
        state.signal_percent = active.signal_percent;
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
