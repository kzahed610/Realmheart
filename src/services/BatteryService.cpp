#include "services/BatteryService.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace realmheart::services {

std::string BatteryService::read_sysfs_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};

    std::string line;
    std::getline(file, line);
    return line;
}

std::optional<BatteryStatus> BatteryService::read() {
    std::error_code error;
    std::filesystem::directory_iterator iterator(power_supply_root_, error);
    if (error) return std::nullopt;

    std::filesystem::path battery_dir;
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const auto filename = iterator->path().filename().string();
        if (filename.starts_with("BAT")) {
            battery_dir = iterator->path();
            break;
        }
        iterator.increment(error);
        if (error) return std::nullopt;
    }

    if (battery_dir.empty()) return std::nullopt;

    try {
        const std::string capacity_text = read_sysfs_file(battery_dir / "capacity");
        std::size_t parsed = 0;
        const int percentage = std::stoi(capacity_text, &parsed);
        if (parsed != capacity_text.size() || percentage < 0 || percentage > 100) {
            return std::nullopt;
        }

        const std::string status = read_sysfs_file(battery_dir / "status");
        if (status.empty()) return std::nullopt;

        return BatteryStatus{percentage, status == "Charging", status};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace realmheart::services
