#include "services/SystemMonitorService.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void require_percentage(double value, const char* name) {
    require(value >= 0.0 && value <= 100.0,
            std::string(name) + " usage must stay within 0-100 percent");
}

} // namespace

int main() {
    const auto snapshot = realmheart::services::SystemMonitorService::read(
        std::chrono::milliseconds(10)
    );
    require(snapshot.has_value(), "/proc usage snapshot must be readable on Linux");
    require_percentage(snapshot->cpu_percent, "CPU");
    require(snapshot->memory_total_kib > 0, "RAM total must be reported");
    require(snapshot->memory_used_kib <= snapshot->memory_total_kib,
            "RAM used must not exceed RAM total");
    require_percentage(snapshot->memory_percent, "RAM");
    require(snapshot->swap_used_kib <= snapshot->swap_total_kib,
            "SWAP used must not exceed SWAP total");
    require_percentage(snapshot->swap_percent, "SWAP");
    if (snapshot->cpu_frequency_mhz) {
        require(*snapshot->cpu_frequency_mhz > 0.0, "CPU frequency must be positive");
    }
    if (snapshot->gpu_percent) require_percentage(*snapshot->gpu_percent, "GPU");
    if (snapshot->gpu_frequency_mhz) {
        require(*snapshot->gpu_frequency_mhz > 0.0, "GPU frequency must be positive");
    }
    std::cout << "System monitor service tests passed\n";
    return 0;
}
