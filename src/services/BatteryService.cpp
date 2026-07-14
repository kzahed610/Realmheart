#include "services/BatteryService.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
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

        std::optional<double> rate_watts;
        const std::string power_now = read_sysfs_file(battery_dir / "power_now");
        if (!power_now.empty()) {
            std::size_t consumed = 0;
            const double microwatts = std::stod(power_now, &consumed);
            if (consumed == power_now.size() && microwatts >= 0.0) {
                rate_watts = microwatts / 1'000'000.0;
            }
        } else {
            const std::string current_now = read_sysfs_file(battery_dir / "current_now");
            const std::string voltage_now = read_sysfs_file(battery_dir / "voltage_now");
            if (!current_now.empty() && !voltage_now.empty()) {
                std::size_t current_consumed = 0;
                std::size_t voltage_consumed = 0;
                const double microamps = std::stod(current_now, &current_consumed);
                const double microvolts = std::stod(voltage_now, &voltage_consumed);
                if (current_consumed == current_now.size() && voltage_consumed == voltage_now.size() &&
                    microamps >= 0.0 && microvolts >= 0.0) {
                    rate_watts = microamps * microvolts / 1'000'000'000'000.0;
                }
            }
        }

        return BatteryStatus{percentage, status == "Charging", status, rate_watts};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace realmheart::services
