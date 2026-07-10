#include "core/Diagnostics.hpp"

#include "core/Command.hpp"

#include <sstream>

namespace realmheart::core {

std::vector<DependencyCheck> collect_dependency_checks() {
    std::vector<DependencyCheck> checks = {
        {"hyprctl", "Hyprland IPC / monitors / clients / dispatch", true, false, {}},
        {"brightnessctl", "Backlight read/write for bar scroll zone and right-sidebar slider", true, false, {}},
        {"wpctl", "PipeWire volume and mute control", true, false, {}},
        {"nmcli", "WiFi status/toggle", true, false, {}},
        {"bluetoothctl", "Bluetooth status/toggle", true, false, {}},
        {"powerprofilesctl", "Battery saver / balanced / performance profile cycling", true, false, {}},
        {"hyprlock", "Reference lock flow until native lock implementation lands", false, false, {}},
        {"hypridle", "Idle inhibition / session behavior integration", false, false, {}},
        {"hyprsunset", "Night Light control", false, false, {}},
        {"matugen", "Wallpaper palette extraction / system theme generation", true, false, {}},
        {"grim", "Screenshot capture backend", false, false, {}},
        {"slurp", "Screenshot region selection fallback", false, false, {}},
        {"wf-recorder", "Overlay recorder backend", false, false, {}}
    };

    for (auto& check : checks) {
        if (auto path = find_in_path(check.name)) {
            check.available = true;
            check.path = *path;
        }
    }
    return checks;
}

std::string format_dependency_report(const std::vector<DependencyCheck>& checks) {
    std::ostringstream out;
    out << "Realmheart dependency report\n";
    out << "============================\n";
    for (const auto& check : checks) {
        out << (check.available ? "[ok]   " : (check.required ? "[MISS] " : "[opt]  "));
        out << check.name << " — " << check.purpose;
        if (check.available) out << " (" << check.path << ")";
        out << '\n';
    }
    return out.str();
}

} // namespace realmheart::core
