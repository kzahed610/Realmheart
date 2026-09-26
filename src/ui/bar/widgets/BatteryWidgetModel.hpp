#pragma once

#include "services/BatteryService.hpp"

#include <algorithm>
#include <string>

namespace realmheart::ui::bar::widgets {

inline std::string battery_icon_path(const services::BatteryStatus& status) {
    const int percentage = std::clamp(status.percentage, 0, 100);
    int level = 0;
    if (percentage >= 88) level = 100;
    else if (percentage >= 63) level = 75;
    else if (percentage >= 38) level = 50;
    else if (percentage >= 13) level = 25;

    if (status.charging) {
        if (level == 0) level = 25;
        return "Realmheart-Icons/battery-charging-" + std::to_string(level) + ".svg";
    }
    return "Realmheart-Icons/battery-" + std::to_string(level) + ".svg";
}

inline std::string format_battery_duration(int total_minutes) {
    const int minutes = std::max(total_minutes, 0);
    const int hours = minutes / 60;
    const int remainder = minutes % 60;
    if (hours == 0) return std::to_string(remainder) + "m";
    if (remainder == 0) return std::to_string(hours) + "h";
    return std::to_string(hours) + "h " + std::to_string(remainder) + "m";
}

} // namespace realmheart::ui::bar::widgets
