#include "services/BatteryService.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>

namespace realmheart::services {

namespace {

std::optional<double> parse_finite_rate(std::string_view value) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(std::string(value), &consumed);
        if (consumed != value.size() || !std::isfinite(parsed) || parsed < 0.0) {
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

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
        if (const auto microwatts = parse_finite_rate(power_now)) {
            rate_watts = *microwatts / 1'000'000.0;
        } else {
            const auto current_now = parse_finite_rate(
                read_sysfs_file(battery_dir / "current_now")
            );
            const auto voltage_now = parse_finite_rate(
                read_sysfs_file(battery_dir / "voltage_now")
            );
            if (current_now && voltage_now) {
                const double watts = *current_now * *voltage_now / 1'000'000'000'000.0;
                if (std::isfinite(watts)) rate_watts = watts;
            }
        }

        return BatteryStatus{percentage, status == "Charging", status, rate_watts};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace realmheart::services
