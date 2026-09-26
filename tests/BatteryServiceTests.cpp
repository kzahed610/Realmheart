#include "services/BatteryService.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_missing_sysfs_directory_is_safe() {
    realmheart::services::BatteryService battery("/definitely/missing/realmheart-power-supply");
    require(!battery.read().has_value(), "missing sysfs root must return no battery");
}

void test_malformed_battery_is_ignored() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-battery-malformed";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "BAT0");
    std::ofstream(root / "BAT0/capacity") << "not-a-number\n";
    std::ofstream(root / "BAT0/status") << "Charging\n";

    realmheart::services::BatteryService battery(root);
    require(!battery.read().has_value(), "malformed capacity must return no battery");
    std::filesystem::remove_all(root);
}

void test_valid_battery_is_read() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-battery-valid";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "BAT1");
    std::ofstream(root / "BAT1/capacity") << "73\n";
    std::ofstream(root / "BAT1/status") << "Charging\n";

    realmheart::services::BatteryService battery(root);
    const auto status = battery.read();
    require(status.has_value(), "valid battery must be detected");
    require(status->percentage == 73, "capacity must be parsed");
    require(status->charging, "charging status must be parsed");
    std::filesystem::remove_all(root);
}

void test_power_rate_is_read_without_extra_processes() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-battery-rate";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "BAT0");
    std::ofstream(root / "BAT0/capacity") << "61\n";
    std::ofstream(root / "BAT0/status") << "Discharging\n";
    std::ofstream(root / "BAT0/power_now") << "12340000\n";

    realmheart::services::BatteryService battery(root);
    const auto status = battery.read();
    require(status.has_value(), "battery with power_now must be readable");
    require(status->rate_watts.has_value(), "power_now must expose a watt rate");
    require(std::abs(*status->rate_watts - 12.34) < 0.001,
            "microwatts must be converted to watts");
    std::filesystem::remove_all(root);
}

void test_current_voltage_rate_fallback_is_read() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-battery-rate-fallback";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "BAT0");
    std::ofstream(root / "BAT0/capacity") << "40\n";
    std::ofstream(root / "BAT0/status") << "Discharging\n";
    std::ofstream(root / "BAT0/current_now") << "2000000\n";
    std::ofstream(root / "BAT0/voltage_now") << "11000000\n";

    realmheart::services::BatteryService battery(root);
    const auto status = battery.read();
    require(status.has_value(), "battery with current and voltage must be readable");
    require(status->rate_watts.has_value(), "current and voltage must produce a watt rate");
    require(std::abs(*status->rate_watts - 22.0) < 0.001,
            "microamp and microvolt values must be converted to watts");
    std::filesystem::remove_all(root);
}

void test_invalid_power_rate_falls_back_to_current_voltage() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-battery-invalid-power";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "BAT0");
    std::ofstream(root / "BAT0/capacity") << "40\n";
    std::ofstream(root / "BAT0/status") << "Discharging\n";
    std::ofstream(root / "BAT0/power_now") << "inf\n";
    std::ofstream(root / "BAT0/current_now") << "2000000\n";
    std::ofstream(root / "BAT0/voltage_now") << "11000000\n";

    realmheart::services::BatteryService battery(root);
    const auto status = battery.read();
    require(status.has_value(), "invalid preferred power source must not hide battery");
    require(status->rate_watts.has_value(), "valid current and voltage must be used as fallback");
    require(std::isfinite(*status->rate_watts), "battery rate must remain finite");
    require(std::abs(*status->rate_watts - 22.0) < 0.001,
            "fallback rate must use current and voltage");
    std::filesystem::remove_all(root);
}

void test_discharge_time_estimate_uses_charge_and_current() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-battery-eta-charge";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "BAT0");
    std::ofstream(root / "BAT0/capacity") << "50\n";
    std::ofstream(root / "BAT0/status") << "Discharging\n";
    std::ofstream(root / "BAT0/charge_now") << "1500000\n";
    std::ofstream(root / "BAT0/current_now") << "-500000\n";
    std::ofstream(root / "BAT0/voltage_now") << "12000000\n";

    realmheart::services::BatteryService battery(root);
    const auto status = battery.read();
    require(status.has_value(), "battery with charge and current readings must be readable");
    require(status->time_remaining_minutes == 180,
            "remaining charge divided by current draw must produce a discharge estimate");
    require(status->rate_watts == 6.0,
            "negative discharge current must still produce a positive power rate");
    require(!status->time_to_full_minutes,
            "discharging battery must not expose a charge-to-full estimate");
    std::filesystem::remove_all(root);
}

void test_charge_time_estimate_uses_remaining_charge() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-battery-eta-full";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "BAT0");
    std::ofstream(root / "BAT0/capacity") << "75\n";
    std::ofstream(root / "BAT0/status") << "Charging\n";
    std::ofstream(root / "BAT0/charge_now") << "1500000\n";
    std::ofstream(root / "BAT0/charge_full") << "2000000\n";
    std::ofstream(root / "BAT0/current_now") << "500000\n";

    realmheart::services::BatteryService battery(root);
    const auto status = battery.read();
    require(status.has_value(), "charging battery with charge readings must be readable");
    require(status->time_to_full_minutes == 60,
            "remaining capacity divided by charge current must estimate time to full");
    require(!status->time_remaining_minutes,
            "charging battery must not expose a discharge estimate");
    std::filesystem::remove_all(root);
}

void test_discharge_time_estimate_supports_energy_and_power() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-battery-eta-energy";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "BAT0");
    std::ofstream(root / "BAT0/capacity") << "50\n";
    std::ofstream(root / "BAT0/status") << "Discharging\n";
    std::ofstream(root / "BAT0/energy_now") << "2000000\n";
    std::ofstream(root / "BAT0/power_now") << "1000000\n";

    realmheart::services::BatteryService battery(root);
    const auto status = battery.read();
    require(status.has_value(), "battery with energy and power readings must be readable");
    require(status->time_remaining_minutes == 120,
            "remaining energy divided by draw power must estimate runtime");
    std::filesystem::remove_all(root);
}

void test_zero_discharge_current_has_no_time_estimate() {
    const auto root = std::filesystem::temp_directory_path() / "realmheart-battery-eta-zero";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "BAT0");
    std::ofstream(root / "BAT0/capacity") << "50\n";
    std::ofstream(root / "BAT0/status") << "Discharging\n";
    std::ofstream(root / "BAT0/charge_now") << "1500000\n";
    std::ofstream(root / "BAT0/current_now") << "0\n";

    realmheart::services::BatteryService battery(root);
    const auto status = battery.read();
    require(status.has_value(), "battery with zero current must still be readable");
    require(!status->time_remaining_minutes,
            "zero current must be reported as unavailable rather than an infinite estimate");
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    test_missing_sysfs_directory_is_safe();
    test_malformed_battery_is_ignored();
    test_valid_battery_is_read();
    test_power_rate_is_read_without_extra_processes();
    test_current_voltage_rate_fallback_is_read();
    test_invalid_power_rate_falls_back_to_current_voltage();
    test_discharge_time_estimate_uses_charge_and_current();
    test_charge_time_estimate_uses_remaining_charge();
    test_discharge_time_estimate_supports_energy_and_power();
    test_zero_discharge_current_has_no_time_estimate();
    std::cout << "BatteryService tests PASSED\n";
    return 0;
}
