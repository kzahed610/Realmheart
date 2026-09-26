#include "services/BatteryService.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

namespace realmheart::services {

namespace {

std::optional<double> parse_finite_number(std::string_view value) {
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(std::string(value), &consumed);
        if (consumed != value.size() || !std::isfinite(parsed)) {
            return std::nullopt;
        }
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> parse_finite_rate(std::string_view value) {
    const auto parsed = parse_finite_number(value);
    if (!parsed || *parsed < 0.0) return std::nullopt;
    return parsed;
}

std::optional<int> estimate_minutes(double remaining, double rate_per_hour) {
    if (!std::isfinite(remaining) || remaining < 0.0 ||
        !std::isfinite(rate_per_hour) || rate_per_hour <= 0.0) {
        return std::nullopt;
    }

    const double minutes = std::ceil((remaining / rate_per_hour) * 60.0);
    if (!std::isfinite(minutes) || minutes > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(minutes);
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

        const auto power_microwatts = parse_finite_rate(
            read_sysfs_file(battery_dir / "power_now")
        );
        const auto current_microamps = parse_finite_number(
            read_sysfs_file(battery_dir / "current_now")
        );
        std::optional<double> rate_watts;
        if (power_microwatts) {
            rate_watts = *power_microwatts / 1'000'000.0;
        } else {
            const auto voltage_now = parse_finite_rate(
                read_sysfs_file(battery_dir / "voltage_now")
            );
            if (current_microamps && voltage_now) {
                const double watts = std::abs(*current_microamps) * *voltage_now /
                    1'000'000'000'000.0;
                if (std::isfinite(watts)) rate_watts = watts;
            }
        }

        const auto energy_now = parse_finite_rate(
            read_sysfs_file(battery_dir / "energy_now")
        );
        const auto energy_full = parse_finite_rate(
            read_sysfs_file(battery_dir / "energy_full")
        );
        const auto charge_now = parse_finite_rate(
            read_sysfs_file(battery_dir / "charge_now")
        );
        const auto charge_full = parse_finite_rate(
            read_sysfs_file(battery_dir / "charge_full")
        );

        std::optional<int> time_remaining_minutes;
        std::optional<int> time_to_full_minutes;
        if (status == "Discharging") {
            if (energy_now && power_microwatts && *power_microwatts > 0.0) {
                time_remaining_minutes = estimate_minutes(*energy_now, *power_microwatts);
            }
            if (!time_remaining_minutes && charge_now && current_microamps &&
                std::abs(*current_microamps) > 0.0) {
                time_remaining_minutes = estimate_minutes(
                    *charge_now,
                    std::abs(*current_microamps)
                );
            }
            if (!time_remaining_minutes && energy_now && rate_watts && *rate_watts > 0.0) {
                time_remaining_minutes = estimate_minutes(
                    *energy_now / 1'000'000.0,
                    *rate_watts
                );
            }
        } else if (status == "Charging") {
            if (energy_now && energy_full) {
                const double remaining_energy = std::max(*energy_full - *energy_now, 0.0);
                if (power_microwatts && *power_microwatts > 0.0) {
                    time_to_full_minutes = estimate_minutes(remaining_energy, *power_microwatts);
                }
                if (!time_to_full_minutes && rate_watts && *rate_watts > 0.0) {
                    time_to_full_minutes = estimate_minutes(
                        remaining_energy / 1'000'000.0,
                        *rate_watts
                    );
                }
            }
            if (!time_to_full_minutes && charge_now && charge_full && current_microamps &&
                std::abs(*current_microamps) > 0.0) {
                const double remaining_charge = std::max(*charge_full - *charge_now, 0.0);
                time_to_full_minutes = estimate_minutes(
                    remaining_charge,
                    std::abs(*current_microamps)
                );
            }
        }

        return BatteryStatus{
            percentage,
            status == "Charging",
            status,
            rate_watts,
            time_remaining_minutes,
            time_to_full_minutes,
        };
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace realmheart::services
