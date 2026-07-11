#include "services/BatteryService.hpp"
#include "services/MediaService.hpp"
#include <iostream>
#include <cassert>

void test_battery_logic() {
    std::cout << "Testing BatteryService..." << std::endl;
    realmheart::services::BatteryService battery;
    auto status = battery.read();
    if (status) {
        std::cout << "Battery detected: " << status->percentage << "% " << status->status << std::endl;
    } else {
        std::cout << "No battery detected (expected on some VMs/Desktops)" << std::endl;
    }
}

void test_media_logic() {
    std::cout << "Testing MediaService..." << std::endl;
    realmheart::services::MediaService media;
    auto m = media.get_current_media();
    if (m) {
        std::cout << "Media detected: " << m->title << " by " << m->artist << std::endl;
    } else {
        std::cout << "No active media detected" << std::endl;
    }
}

int main() {
    test_battery_logic();
    test_media_logic();
    return 0;
}
