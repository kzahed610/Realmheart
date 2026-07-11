#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace realmheart::services {

struct BatteryStatus {
    int percentage;
    bool charging;
    std::string status;
};

class BatteryService {
public:
    BatteryService() = default;
    explicit BatteryService(std::filesystem::path power_supply_root)
        : power_supply_root_(std::move(power_supply_root)) {}
    ~BatteryService() = default;

    std::optional<BatteryStatus> read();

private:
    std::filesystem::path power_supply_root_ = "/sys/class/power_supply";

    static std::string read_sysfs_file(const std::filesystem::path& path);
};

} // namespace realmheart::services
