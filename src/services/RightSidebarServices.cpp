#include "RightSidebarServices.hpp"

#include "core/Command.hpp"
#include "services/Brightness.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realmheart::services {

namespace {

std::string unavailable(const std::string& reason) {
    return "Unavailable: " + reason;
}

std::string yes_no_state(bool enabled, const std::string& enabled_text, const std::string& disabled_text) {
    return enabled ? enabled_text : disabled_text;
}

std::string format_percent(double percent) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << percent << '%';
    return out.str();
}

bool usable_output(const realmheart::core::CommandResult& result) {
    return result.succeeded() && !result.truncated && !result.output.empty();
}

std::string command_unavailable(
    const realmheart::core::CommandResult& result,
    std::string_view fallback
) {
    return unavailable(realmheart::core::command_failure_detail(result, fallback));
}

std::string first_active_wifi_ssid(const realmheart::core::CommandOptions& options) {
    const auto scan = realmheart::core::run_capture(
        {"nmcli", "-t", "-f", "ACTIVE,SSID", "dev", "wifi"},
        options
    );
    if (!usable_output(scan)) return {};

    std::stringstream lines(scan.output);
    std::string line;
    while (std::getline(lines, line)) {
        constexpr std::string_view prefix = "yes:";
        if (line.rfind(std::string(prefix), 0) == 0 && line.size() > prefix.size()) {
            return realmheart::core::sanitize_command_detail(line.substr(prefix.size()), 96);
        }
    }
    return {};
}

bool output_contains(const std::string& output, const std::string& needle) {
    return output.find(needle) != std::string::npos;
}

} // namespace

RightSidebarServices::RightSidebarServices(realmheart::core::CommandOptions command_options)
    : command_options_(std::move(command_options)) {}

ServiceStatus RightSidebarServices::getWifiStatus() const {
    if (!realmheart::core::command_exists("nmcli")) {
        return {"WiFi", unavailable("nmcli not found"), false};
    }

    const auto radio = realmheart::core::run_capture({"nmcli", "radio", "wifi"}, command_options_);
    if (!usable_output(radio)) {
        return {"WiFi", command_unavailable(radio, "nmcli radio wifi failed"), false};
    }

    const bool enabled = radio.output == "enabled";
    std::string status = enabled ? "Enabled" : "Disabled";
    if (enabled) {
        if (auto ssid = first_active_wifi_ssid(command_options_); !ssid.empty()) status += " (" + ssid + ")";
        else status += " (not connected)";
    }
    return {"WiFi", status, enabled};
}

ServiceStatus RightSidebarServices::getBluetoothStatus() const {
    if (!realmheart::core::command_exists("bluetoothctl")) {
        return {"Bluetooth", unavailable("bluetoothctl not found"), false};
    }

    const auto show = realmheart::core::run_capture({"bluetoothctl", "show"}, command_options_);
    if (!usable_output(show)) {
        return {"Bluetooth", command_unavailable(show, "bluetoothctl show failed"), false};
    }

    const bool powered = output_contains(show.output, "Powered: yes");
    return {"Bluetooth", yes_no_state(powered, "Powered", "Not powered"), powered};
}

ServiceStatus RightSidebarServices::getKeepAwakeStatus() const {
    if (!realmheart::core::command_exists("hypridle")) {
        return {"Keep Awake", unavailable("hypridle not found"), false};
    }
    return {"Keep Awake", unavailable("no safe read-only idle-inhibit state probe available"), false};
}

ServiceStatus RightSidebarServices::getNightLightStatus() const {
    if (!realmheart::core::command_exists("hyprsunset")) {
        return {"Night Light", unavailable("hyprsunset not found"), false};
    }

    const auto running = realmheart::core::run_capture({"pgrep", "-x", "hyprsunset"}, command_options_);
    if (running.succeeded() && !running.output.empty()) return {"Night Light", "Running", true};
    if (running.status == realmheart::core::CommandStatus::Exited && running.exit_code == 1) {
        return {"Night Light", "Not running", false};
    }
    return {"Night Light", command_unavailable(running, "pgrep hyprsunset failed"), false};
}

ServiceStatus RightSidebarServices::getGamemodeStatus() const {
    if (!realmheart::core::command_exists("gamemoded")) {
        return {"Gamemode", unavailable("gamemoded not found"), false};
    }

    const auto status = realmheart::core::run_capture({"gamemoded", "-s"}, command_options_);
    if (!usable_output(status)) {
        return {"Gamemode", command_unavailable(status, "gamemoded -s failed"), false};
    }

    const bool active = output_contains(status.output, "GameMode is active")
        || (output_contains(status.output, "active") && !output_contains(status.output, "inactive"));
    return {"Gamemode", realmheart::core::sanitize_command_detail(status.output), active};
}

ServiceStatus RightSidebarServices::getPowerProfileStatus() const {
    if (!realmheart::core::command_exists("powerprofilesctl")) {
        return {"Power Profile", unavailable("powerprofilesctl not found"), false};
    }

    const auto profile = realmheart::core::run_capture({"powerprofilesctl", "get"}, command_options_);
    if (!usable_output(profile)) {
        return {"Power Profile", command_unavailable(profile, "powerprofilesctl unavailable or returned no profile"), false};
    }
    return {"Power Profile", realmheart::core::sanitize_command_detail(profile.output), true};
}

ServiceStatus RightSidebarServices::getBrightnessStatus() const {
    auto brightness = Brightness::read(command_options_);
    if (!brightness) return {"Brightness", unavailable("brightnessctl unavailable or unreadable"), false};
    return {"Brightness", format_percent(brightness->percent), brightness->maximum > 0};
}

ServiceStatus RightSidebarServices::getVolumeStatus() const {
    if (!realmheart::core::command_exists("wpctl")) {
        return {"Volume", unavailable("wpctl not found"), false};
    }

    const auto audio = realmheart::core::run_capture(
        {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"},
        command_options_
    );
    if (!usable_output(audio)) {
        return {"Volume", command_unavailable(audio, "wpctl default sink unreadable"), false};
    }
    return {"Volume", realmheart::core::sanitize_command_detail(audio.output), true};
}

ServiceStatus RightSidebarServices::getNotificationsStatus() const {
    return {"Notifications", unavailable("notification history pending; confirmed right-sidebar scope"), false};
}

std::vector<ServiceStatus> RightSidebarServices::getBarStatus() const {
    std::vector<ServiceStatus> bar_status;
    bar_status.push_back(getWifiStatus());
    bar_status.push_back(getBluetoothStatus());
    bar_status.push_back(getKeepAwakeStatus());
    bar_status.push_back(getNightLightStatus());
    bar_status.push_back(getGamemodeStatus());
    bar_status.push_back(getPowerProfileStatus());
    bar_status.push_back(getBrightnessStatus());
    bar_status.push_back(getVolumeStatus());
    return bar_status;
}

std::vector<ServiceStatus> RightSidebarServices::getReport() const {
    std::vector<ServiceStatus> report;
    report.push_back(getWifiStatus());
    report.push_back(getBluetoothStatus());
    report.push_back(getKeepAwakeStatus());
    report.push_back(getNightLightStatus());
    report.push_back(getGamemodeStatus());
    report.push_back(getPowerProfileStatus());
    report.push_back(getBrightnessStatus());
    report.push_back(getVolumeStatus());
    report.push_back(getNotificationsStatus());
    return report;
}

void RightSidebarServices::printReport() const {
    std::cout << "Right Sidebar Services Report:\n";
    for (const auto& service : getReport()) {
        std::cout << "  " << service.name << ": " << service.status;
        if (service.enabled) {
            std::cout << " (Enabled)";
        } else {
            std::cout << " (Disabled)";
        }
        std::cout << "\n";
    }
}

} // namespace realmheart::services
