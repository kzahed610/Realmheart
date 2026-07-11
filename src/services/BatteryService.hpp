#pragma once

#include <string>
#include <optional>

namespace realmheart::services {

struct BatteryStatus {
    int percentage;
    bool charging;
    std::string status; // e.g., "Charging", "Discharging", "Full"
};

class BatteryService {
public:
    BatteryService() = default;
    ~BatteryService() = default;

    // Read the current battery status from /sys/class/power_supply/
    std::optional<BatteryStatus> read();

private:
    std::string read_sysfs_file(const std::string& path);
};

} // namespace realmheart::services
