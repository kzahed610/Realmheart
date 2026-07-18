#include "services/Wifi.hpp"

#include "core/Command.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <sstream>
#include <string_view>
#include <unordered_map>
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

std::unordered_map<std::string, std::string> saved_connections(
    const realmheart::core::CommandOptions& options
) {
    std::unordered_map<std::string, std::string> saved;
    const auto result = realmheart::core::run_capture(
        {"nmcli", "-t", "-f", "NAME,UUID,TYPE", "connection", "show"},
        options
    );
    if (!result.succeeded() || result.truncated) return saved;

    std::stringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = split_nmcli_fields(line);
        if (fields.size() < 3 || fields[0].empty() || fields[1].empty()) continue;
        if (fields[2] != "802-11-wireless" && fields[2] != "wifi") continue;
        saved.emplace(fields[0], fields[1]);
    }
    return saved;
}

std::optional<std::string> connected_wifi_device(
    const realmheart::core::CommandOptions& options
) {
    const auto result = realmheart::core::run_capture(
        {"nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device"},
        options
    );
    if (!result.succeeded() || result.truncated) return std::nullopt;

    std::stringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = split_nmcli_fields(line);
        if (fields.size() < 3 || fields[1] != "wifi") continue;
        if (fields[2] == "connected" || fields[2] == "connecting") {
            return fields[0];
        }
    }
    return std::nullopt;
}

WifiMutationResult failed_mutation(
    const realmheart::core::CommandResult& result,
    std::string_view fallback
) {
    WifiMutationResult mutation;
    mutation.error = realmheart::core::command_failure_detail(result, fallback);
    return mutation;
}

struct ActiveNetwork {
    std::string ssid;
    std::optional<int> signal_percent;
};

ActiveNetwork active_network(const realmheart::core::CommandOptions& options) {
    const auto scan = realmheart::core::run_capture(
        {
            "nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL",
            "device", "wifi", "list", "--rescan", "no"
        },
        options
    );
    if (usable_output(scan)) {
        std::stringstream lines(scan.output);
        std::string line;
        while (std::getline(lines, line)) {
            const auto fields = split_nmcli_fields(line);
            if (fields.size() < 2) continue;
            const bool active = fields[0] == "*" || fields[0] == "yes" ||
                fields[0] == "true";
            if (!active) continue;

            ActiveNetwork network;
            network.ssid = realmheart::core::sanitize_command_detail(fields[1], 96);
            if (fields.size() >= 3) network.signal_percent = parse_signal(fields[2]);
            return network;
        }
    }

    // NetworkManager can briefly omit the IN-USE marker immediately after a
    // resume or radio transition even while the device already reports a
    // connected profile. Use the device-status connection name as a fallback
    // instead of incorrectly presenting an active connection as disconnected.
    const auto devices = realmheart::core::run_capture(
        {"nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status"},
        options
    );
    if (!devices.succeeded() || devices.truncated) return {};

    std::stringstream lines(devices.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = split_nmcli_fields(line);
        if (fields.size() < 4 || fields[1] != "wifi") continue;
        if (fields[2] != "connected" && fields[2] != "connecting") continue;
        if (fields[3].empty() || fields[3] == "--") continue;
        return {
            .ssid = realmheart::core::sanitize_command_detail(fields[3], 96),
            .signal_percent = std::nullopt,
        };
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

std::vector<WifiNetwork> Wifi::scan(
    bool rescan,
    const realmheart::core::CommandOptions& options
) {
    if (!realmheart::core::command_exists("nmcli")) return {};

    const auto result = realmheart::core::run_capture(
        {
            "nmcli", "-t", "-f", "IN-USE,SSID,BSSID,SIGNAL,SECURITY",
            "device", "wifi", "list", "--rescan", rescan ? "yes" : "no"
        },
        options
    );
    if (!result.succeeded() || result.truncated) return {};

    const auto saved = saved_connections(options);
    std::map<std::string, WifiNetwork> strongest_by_ssid;
    std::stringstream lines(result.output);
    std::string line;
    while (std::getline(lines, line)) {
        const auto fields = split_nmcli_fields(line);
        if (fields.size() < 5 || fields[1].empty()) continue;

        WifiNetwork network;
        network.active = fields[0] == "*" || fields[0] == "yes";
        network.ssid = realmheart::core::sanitize_command_detail(fields[1], 96);
        network.bssid = realmheart::core::sanitize_command_detail(fields[2], 32);
        network.signal_percent = parse_signal(fields[3]).value_or(0);
        network.security = realmheart::core::sanitize_command_detail(fields[4], 48);
        if (const auto existing = saved.find(network.ssid); existing != saved.end()) {
            network.saved = true;
            network.connection_uuid = existing->second;
        }

        auto candidate = strongest_by_ssid.find(network.ssid);
        if (candidate == strongest_by_ssid.end() || network.active ||
            (!candidate->second.active && network.signal_percent > candidate->second.signal_percent)) {
            strongest_by_ssid[network.ssid] = std::move(network);
        }
    }

    std::vector<WifiNetwork> networks;
    networks.reserve(strongest_by_ssid.size());
    for (auto& [_, network] : strongest_by_ssid) networks.push_back(std::move(network));
    std::sort(networks.begin(), networks.end(), [](const auto& left, const auto& right) {
        if (left.active != right.active) return left.active > right.active;
        if (left.signal_percent != right.signal_percent) {
            return left.signal_percent > right.signal_percent;
        }
        return left.ssid < right.ssid;
    });
    return networks;
}

WifiMutationResult Wifi::connect(
    const std::string& ssid,
    const std::optional<std::string>& password,
    const std::string& connection_uuid,
    const realmheart::core::CommandOptions& options
) {
    WifiMutationResult mutation;
    if (!realmheart::core::command_exists("nmcli")) {
        mutation.error = "nmcli not found";
        return mutation;
    }
    if (ssid.empty()) {
        mutation.error = "WiFi network name is empty";
        return mutation;
    }

    std::vector<std::string> command;
    if (!connection_uuid.empty()) {
        command = {"nmcli", "connection", "up", "uuid", connection_uuid};
    } else {
        command = {"nmcli", "device", "wifi", "connect", ssid};
        if (password && !password->empty()) {
            command.emplace_back("password");
            command.push_back(*password);
        }
    }

    const auto write = realmheart::core::run_capture(command, options);
    if (!write.succeeded()) return failed_mutation(write, "WiFi connection failed");

    const auto readback = read(options);
    if (!readback) {
        mutation.error = "WiFi state unavailable after connection";
        return mutation;
    }
    mutation.state = *readback;
    mutation.success = mutation.state.enabled && mutation.state.ssid == ssid;
    if (!mutation.success) mutation.error = "WiFi did not connect to the requested network";
    return mutation;
}

WifiMutationResult Wifi::disconnect(const realmheart::core::CommandOptions& options) {
    WifiMutationResult mutation;
    if (!realmheart::core::command_exists("nmcli")) {
        mutation.error = "nmcli not found";
        return mutation;
    }

    const auto device = connected_wifi_device(options);
    if (!device) {
        const auto state = read(options);
        if (state) mutation.state = *state;
        mutation.success = state.has_value() && state->ssid.empty();
        if (!mutation.success) mutation.error = "No connected WiFi device found";
        return mutation;
    }

    const auto write = realmheart::core::run_capture(
        {"nmcli", "device", "disconnect", *device}, options
    );
    if (!write.succeeded()) return failed_mutation(write, "WiFi disconnect failed");

    const auto readback = read(options);
    if (!readback) {
        mutation.error = "WiFi state unavailable after disconnect";
        return mutation;
    }
    mutation.state = *readback;
    mutation.success = mutation.state.ssid.empty();
    if (!mutation.success) mutation.error = "WiFi remained connected after disconnect";
    return mutation;
}

WifiMutationResult Wifi::forget(
    const std::string& ssid,
    const std::string& connection_uuid,
    const realmheart::core::CommandOptions& options
) {
    WifiMutationResult mutation;
    if (!realmheart::core::command_exists("nmcli")) {
        mutation.error = "nmcli not found";
        return mutation;
    }
    if (ssid.empty() && connection_uuid.empty()) {
        mutation.error = "WiFi connection identifier is empty";
        return mutation;
    }

    const std::vector<std::string> command = !connection_uuid.empty()
        ? std::vector<std::string>{"nmcli", "connection", "delete", "uuid", connection_uuid}
        : std::vector<std::string>{"nmcli", "connection", "delete", "id", ssid};
    const auto write = realmheart::core::run_capture(command, options);
    if (!write.succeeded()) return failed_mutation(write, "Forgetting WiFi network failed");

    if (const auto readback = read(options)) mutation.state = *readback;
    mutation.success = true;
    return mutation;
}

} // namespace realmheart::services
