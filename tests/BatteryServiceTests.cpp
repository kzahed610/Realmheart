#include "services/BatteryService.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

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

} // namespace

int main() {
    test_missing_sysfs_directory_is_safe();
    test_malformed_battery_is_ignored();
    test_valid_battery_is_read();
    std::cout << "BatteryService tests PASSED\n";
    return 0;
}
