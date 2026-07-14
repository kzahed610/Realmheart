#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace realmheart::services {

struct SystemUsageSnapshot {
    double cpu_percent = 0.0;
    std::optional<double> cpu_frequency_mhz;

    std::uint64_t memory_used_kib = 0;
    std::uint64_t memory_total_kib = 0;
    double memory_percent = 0.0;

    std::uint64_t swap_used_kib = 0;
    std::uint64_t swap_total_kib = 0;
    double swap_percent = 0.0;

    std::optional<double> gpu_percent;
    std::optional<double> gpu_frequency_mhz;
};

class SystemMonitorService {
public:
    // Samples only on demand. No permanent process or timer is kept alive.
    static std::optional<SystemUsageSnapshot> read(
        std::chrono::milliseconds cpu_sample_interval = std::chrono::milliseconds(120)
    );
};

} // namespace realmheart::services
