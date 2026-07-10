#include "RightSidebarServices.hpp"

#include "core/Command.hpp"
#include "services/Audio.hpp"
#include "services/Bluetooth.hpp"
#include "services/Brightness.hpp"
#include "services/GameMode.hpp"
#include "services/KeepAwake.hpp"
#include "services/NightLight.hpp"
#include "services/PowerProfiles.hpp"
#include "services/Wifi.hpp"

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

} // namespace

RightSidebarServices::RightSidebarServices(realmheart::core::CommandOptions command_options)
    : command_options_(std::move(command_options)) {}

ServiceStatus RightSidebarServices::getWifiStatus() const {
    const auto wifi = Wifi::read(command_options_);
    if (!wifi) return {"WiFi", unavailable("nmcli unavailable or unreadable"), false};

    std::string status = wifi->enabled ? "Enabled" : "Disabled";
    if (wifi->enabled) {
        status += wifi->ssid.empty() ? " (not connected)" : " (" + wifi->ssid + ")";
    }
    return {"WiFi", status, wifi->enabled};
}

ServiceStatus RightSidebarServices::getBluetoothStatus() const {
    const auto bluetooth = Bluetooth::read(command_options_);
    if (!bluetooth) {
        return {"Bluetooth", unavailable("bluetoothctl unavailable or unreadable"), false};
    }
    return {
        "Bluetooth",
        yes_no_state(bluetooth->powered, "Powered", "Not powered"),
        bluetooth->powered
    };
}

ServiceStatus RightSidebarServices::getKeepAwakeStatus() const {
    KeepAwake keep_awake;
    const bool active = keep_awake.active(command_options_);
    return {"Keep Awake", yes_no_state(active, "Inhibiting idle", "Idle allowed"), active};
}

ServiceStatus RightSidebarServices::getNightLightStatus() const {
    const auto night_light = NightLight::read(command_options_);
    if (!night_light) return {"Night Light", unavailable("hyprsunset IPC unavailable"), false};
    return {
        "Night Light",
        std::to_string(night_light->temperature) + "K",
        night_light->enabled
    };
}

ServiceStatus RightSidebarServices::getGamemodeStatus() const {
    const auto gamemode = GameMode::read(command_options_);
    if (!gamemode) return {"Gamemode", unavailable("Hyprland option unreadable"), false};
    return {
        "Gamemode",
        yes_no_state(gamemode->enabled, "Compositor effects disabled", "Normal compositor settings"),
        gamemode->enabled
    };
}

ServiceStatus RightSidebarServices::getPowerProfileStatus() const {
    const auto profile = PowerProfiles::current();
    if (!profile) return {"Power Profile", unavailable("powerprofilesctl unavailable"), false};
    return {"Power Profile", *profile, true};
}

ServiceStatus RightSidebarServices::getBrightnessStatus() const {
    auto brightness = Brightness::read(command_options_);
    if (!brightness) return {"Brightness", unavailable("brightnessctl unavailable or unreadable"), false};
    return {"Brightness", format_percent(brightness->percent), brightness->maximum > 0};
}

ServiceStatus RightSidebarServices::getVolumeStatus() const {
    const auto audio = Audio::read_default_sink(command_options_);
    if (!audio) return {"Volume", unavailable("wpctl unavailable or unreadable"), false};

    std::string status = format_percent(audio->volume * 100.0);
    if (audio->muted) status += " (Muted)";
    return {"Volume", status, true};
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
