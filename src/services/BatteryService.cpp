#include "services/BatteryService.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>

namespace realmheart::services {

std::string BatteryService::read_sysfs_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::string line;
    std::getline(file, line);
    return line;
}

std::optional<BatteryStatus> BatteryService::read() {
    // In Linux, battery info is in /sys/class/power_supply/BAT0 or BAT1
    std::string battery_dir;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/power_supply/")) {
        if (entry.path().filename().string().find("BAT") == 0) {
            battery_dir = entry.path().string();
            break;
        }
    }

    if (battery_dir.empty()) {
        return std::nullopt;
    }

    try {
        int percentage = std::stoi(read_sysfs_file(battery_dir + "/capacity"));
        std::string status = read_sysfs_file(battery_dir + "/status");
        bool charging = (status == "Charging");

        return BatteryStatus{percentage, charging, status};
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace realmheart::services
