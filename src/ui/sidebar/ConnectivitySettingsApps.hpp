#pragma once

#include <array>
#include <span>
#include <string_view>

namespace realmheart::ui::sidebar {

enum class ConnectivitySettingsService {
    Wifi,
    Bluetooth,
};

struct SettingsAppCandidate {
    std::string_view desktop_id;
    std::string_view executable;
    std::string_view argument;
};

enum class SettingsAppLaunchStatus {
    Opened,
    Unavailable,
    Failed,
};

inline constexpr std::array<SettingsAppCandidate, 3> kWifiSettingsApps{{
    {"nm-connection-editor.desktop", {}, {}},
    {"kcm_networkmanagement.desktop", {}, {}},
    {{}, "gnome-control-center", "wifi"},
}};

inline constexpr std::array<SettingsAppCandidate, 3> kBluetoothSettingsApps{{
    {"kcm_bluetooth.desktop", {}, {}},
    {"blueman-manager.desktop", {}, {}},
    {{}, "gnome-control-center", "bluetooth"},
}};

inline constexpr std::span<const SettingsAppCandidate> connectivity_settings_candidates(
    ConnectivitySettingsService service
) noexcept {
    if (service == ConnectivitySettingsService::Wifi) return kWifiSettingsApps;
    return kBluetoothSettingsApps;
}

inline constexpr std::string_view connectivity_settings_install_suggestion(
    ConnectivitySettingsService service
) noexcept {
    if (service == ConnectivitySettingsService::Wifi) {
        return "Not installed — install nm-connection-editor, plasma-nm, or gnome-control-center.";
    }
    return "Not installed — install blueman, bluedevil, or gnome-control-center.";
}

template <typename IsInstalled, typename Launch>
SettingsAppLaunchStatus launch_available_connectivity_settings(
    ConnectivitySettingsService service,
    IsInstalled&& is_installed,
    Launch&& launch
) {
    bool found_installed_app = false;
    for (const SettingsAppCandidate& candidate : connectivity_settings_candidates(service)) {
        if (!is_installed(candidate)) continue;
        found_installed_app = true;
        if (launch(candidate)) return SettingsAppLaunchStatus::Opened;
    }
    return found_installed_app
        ? SettingsAppLaunchStatus::Failed
        : SettingsAppLaunchStatus::Unavailable;
}

} // namespace realmheart::ui::sidebar
