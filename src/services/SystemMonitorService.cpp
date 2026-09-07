#include "services/SystemMonitorService.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace realmheart::services {
namespace {

constexpr std::size_t kMaxDrmProcesses = 256;
constexpr std::size_t kMaxDrmFdinfoEntries = 2048;

struct CpuCounters {
    unsigned long long idle = 0;
    unsigned long long total = 0;
};

std::optional<CpuCounters> read_cpu_counters() {
    std::ifstream input("/proc/stat");
    std::string line;
    if (!std::getline(input, line) || !line.starts_with("cpu ")) return std::nullopt;

    std::istringstream stream(line.substr(4));
    std::array<unsigned long long, 10> fields{};
    std::size_t count = 0;
    while (count < fields.size() && stream >> fields[count]) ++count;
    if (count < 4) return std::nullopt;

    CpuCounters counters;
    counters.idle = fields[3] + (count > 4 ? fields[4] : 0);
    for (std::size_t index = 0; index < count; ++index) counters.total += fields[index];
    return counters;
}

struct MemoryCounters {
    unsigned long long total_kib = 0;
    unsigned long long available_kib = 0;
    unsigned long long swap_total_kib = 0;
    unsigned long long swap_free_kib = 0;
};

std::optional<MemoryCounters> read_memory_counters() {
    std::ifstream input("/proc/meminfo");
    if (!input) return std::nullopt;

    MemoryCounters counters;
    bool saw_total = false;
    bool saw_available = false;
    bool saw_swap_total = false;
    bool saw_swap_free = false;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string key;
        std::string value_text;
        std::string unit;
        if (!(fields >> key)) continue;
        if (key != "MemTotal:" && key != "MemAvailable:" &&
            key != "SwapTotal:" && key != "SwapFree:") {
            continue;
        }
        if (!(fields >> value_text >> unit) || unit != "kB") return std::nullopt;
        std::string extra;
        if (fields >> extra) return std::nullopt;

        unsigned long long value = 0;
        const auto [end, error] = std::from_chars(
            value_text.data(), value_text.data() + value_text.size(), value
        );
        if (error != std::errc{} || end != value_text.data() + value_text.size()) {
            return std::nullopt;
        }
        if (key == "MemTotal:") {
            counters.total_kib = value;
            saw_total = true;
        } else if (key == "MemAvailable:") {
            counters.available_kib = value;
            saw_available = true;
        } else if (key == "SwapTotal:") {
            counters.swap_total_kib = value;
            saw_swap_total = true;
        } else if (key == "SwapFree:") {
            counters.swap_free_kib = value;
            saw_swap_free = true;
        }
    }
    if (!saw_total || !saw_available || counters.total_kib == 0) return std::nullopt;
    if (saw_swap_total != saw_swap_free) return std::nullopt;
    return counters;
}

std::optional<unsigned long long> read_unsigned_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    if (!std::getline(input, value)) return std::nullopt;
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return std::nullopt;
    value = value.substr(first, last - first + 1);
    unsigned long long parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed
    );
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return parsed;
}

std::string trim_copy(std::string_view value);

bool is_cpu_directory(std::string_view name) {
    if (!name.starts_with("cpu") || name.size() <= 3) return false;
    return std::all_of(name.begin() + 3, name.end(), [](char character) {
        return std::isdigit(static_cast<unsigned char>(character)) != 0;
    });
}

std::optional<double> read_cpu_frequency_mhz() {
    std::error_code error;
    const std::filesystem::path cpu_root("/sys/devices/system/cpu");
    unsigned long long total_khz = 0;
    unsigned int samples = 0;

    std::filesystem::directory_iterator iterator(cpu_root, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const auto entry = *iterator;
        const auto name = entry.path().filename().string();
        if (is_cpu_directory(name)) {
            const auto value = read_unsigned_file(entry.path() / "cpufreq" / "scaling_cur_freq");
            if (value && *value > 0) {
                total_khz += *value;
                ++samples;
            }
        }
        iterator.increment(error);
    }
    if (samples > 0) {
        return static_cast<double>(total_khz) / static_cast<double>(samples) / 1000.0;
    }

    // Some kernels omit cpufreq sysfs for a subset of drivers. /proc/cpuinfo
    // remains a cheap, read-only fallback for the currently reported clocks.
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    double total_mhz = 0.0;
    samples = 0;
    while (std::getline(cpuinfo, line)) {
        if (!line.starts_with("cpu MHz")) continue;
        const auto separator = line.find(':');
        if (separator == std::string::npos) continue;
        const auto raw = trim_copy(std::string_view(line).substr(separator + 1));
        try {
            std::size_t consumed = 0;
            const double value = std::stod(raw, &consumed);
            if (consumed != raw.size() || !std::isfinite(value) || value <= 0.0) continue;
            total_mhz += value;
            ++samples;
        } catch (...) {
            continue;
        }
    }
    if (samples == 0) return std::nullopt;
    return total_mhz / static_cast<double>(samples);
}


struct DrmEngineCounter {
    unsigned long long busy_ns = 0;
    std::string device_engine_key;
    unsigned int capacity = 1;
};

struct DrmEngineSnapshot {
    std::unordered_map<std::string, DrmEngineCounter> clients;
};

bool is_numeric_name(std::string_view name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](char character) {
        return std::isdigit(static_cast<unsigned char>(character)) != 0;
    });
}

std::string trim_copy(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::optional<unsigned long long> parse_leading_unsigned(std::string_view value) {
    std::istringstream stream{std::string(value)};
    unsigned long long parsed = 0;
    if (!(stream >> parsed)) return std::nullopt;
    return parsed;
}

void read_drm_fdinfo_file(
    const std::filesystem::path& path,
    DrmEngineSnapshot& snapshot
) {
    std::ifstream input(path);
    if (!input) return;

    std::string driver;
    std::string pdev;
    std::string client_id;
    std::unordered_map<std::string, unsigned long long> engines;
    std::unordered_map<std::string, unsigned int> capacities;
    std::string line;

    while (std::getline(input, line)) {
        const auto delimiter = line.find(':');
        if (delimiter == std::string::npos) continue;
        const std::string key = trim_copy(std::string_view(line).substr(0, delimiter));
        const std::string value = trim_copy(std::string_view(line).substr(delimiter + 1));

        if (key == "drm-driver") {
            driver = value;
        } else if (key == "drm-pdev") {
            pdev = value;
        } else if (key == "drm-client-id") {
            std::istringstream stream(value);
            stream >> client_id;
        } else if (key.starts_with("drm-engine-capacity-")) {
            const auto parsed = parse_leading_unsigned(value);
            if (parsed && *parsed > 0) {
                capacities[key.substr(std::string_view("drm-engine-capacity-").size())] =
                    static_cast<unsigned int>(std::min<unsigned long long>(*parsed, 1024));
            }
        } else if (key.starts_with("drm-engine-")) {
            const auto parsed = parse_leading_unsigned(value);
            if (parsed) {
                engines[key.substr(std::string_view("drm-engine-").size())] = *parsed;
            }
        }
    }

    if (driver.empty() || client_id.empty() || engines.empty()) return;
    const std::string device = pdev.empty() ? driver : pdev;
    for (const auto& [engine, busy_ns] : engines) {
        const std::string device_engine_key = device + '|' + engine;
        const std::string client_engine_key = device + '|' + client_id + '|' + engine;
        const unsigned int capacity = capacities.contains(engine) ? capacities.at(engine) : 1;

        auto [iterator, inserted] = snapshot.clients.emplace(
            client_engine_key,
            DrmEngineCounter{busy_ns, device_engine_key, capacity}
        );
        if (!inserted && busy_ns > iterator->second.busy_ns) {
            iterator->second.busy_ns = busy_ns;
            iterator->second.capacity = capacity;
        }
    }
}

DrmEngineSnapshot read_drm_engine_snapshot() {
    DrmEngineSnapshot snapshot;
    std::error_code process_error;
    std::filesystem::directory_iterator process_iterator("/proc", process_error);
    const std::filesystem::directory_iterator end;

    std::size_t process_count = 0;
    std::size_t fdinfo_count = 0;
    while (!process_error && process_iterator != end &&
           process_count < kMaxDrmProcesses &&
           fdinfo_count < kMaxDrmFdinfoEntries) {
        const auto process_path = process_iterator->path();
        const auto process_name = process_path.filename().string();
        process_iterator.increment(process_error);
        if (!is_numeric_name(process_name)) continue;
        ++process_count;

        std::error_code fd_error;
        std::filesystem::directory_iterator fd_iterator(process_path / "fdinfo", fd_error);
        while (!fd_error && fd_iterator != end &&
               fdinfo_count < kMaxDrmFdinfoEntries) {
            const auto fdinfo_path = fd_iterator->path();
            fd_iterator.increment(fd_error);
            ++fdinfo_count;
            read_drm_fdinfo_file(fdinfo_path, snapshot);
        }
    }
    return snapshot;
}

std::optional<double> calculate_drm_usage(
    const DrmEngineSnapshot& first,
    const DrmEngineSnapshot& second,
    std::chrono::steady_clock::duration elapsed
) {
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    if (elapsed_ns <= 0) return std::nullopt;

    struct EngineAggregate {
        unsigned long long busy_delta_ns = 0;
        unsigned int capacity = 1;
    };
    std::unordered_map<std::string, EngineAggregate> aggregates;

    for (const auto& [client_key, current] : second.clients) {
        const auto previous = first.clients.find(client_key);
        if (previous == first.clients.end() || current.busy_ns < previous->second.busy_ns) continue;

        auto& aggregate = aggregates[current.device_engine_key];
        aggregate.busy_delta_ns += current.busy_ns - previous->second.busy_ns;
        aggregate.capacity = std::max(aggregate.capacity, current.capacity);
    }

    std::optional<double> busiest_engine;
    for (const auto& [_, aggregate] : aggregates) {
        const double denominator = static_cast<double>(elapsed_ns) *
            static_cast<double>(std::max(1U, aggregate.capacity));
        if (denominator <= 0.0) continue;
        const double usage = std::clamp(
            100.0 * static_cast<double>(aggregate.busy_delta_ns) / denominator,
            0.0,
            100.0
        );
        busiest_engine = busiest_engine ? std::max(*busiest_engine, usage) : usage;
    }
    return busiest_engine;
}

std::optional<double> parse_percentage_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string value;
    if (!std::getline(input, value)) return std::nullopt;
    value = trim_copy(value);

    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(value, &consumed);
        if (consumed != value.size() || !std::isfinite(parsed) || parsed < 0.0 || parsed > 100.0) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        return std::nullopt;
    }
}

struct GpuMetrics {
    std::optional<double> usage_percent;
    std::optional<double> frequency_mhz;
};

GpuMetrics read_gpu_metrics() {
    GpuMetrics metrics;
    std::error_code error;
    const std::filesystem::path drm_root("/sys/class/drm");
    std::filesystem::directory_iterator iterator(drm_root, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const auto entry = *iterator;
        const auto name = entry.path().filename().string();
        if (name.starts_with("card") && name.find('-') == std::string::npos) {
            const auto device = entry.path() / "device";
            if (!metrics.usage_percent) {
                for (const auto& candidate : {
                         device / "gpu_busy_percent",
                         device / "busy_percent",
                         device / "gt_busy_percent"
                     }) {
                    if (const auto usage = parse_percentage_file(candidate)) {
                        metrics.usage_percent = usage;
                        break;
                    }
                }
            }

            if (!metrics.frequency_mhz) {
                for (const auto& candidate : {
                         entry.path() / "gt_cur_freq_mhz",
                         entry.path() / "gt" / "gt0" / "rps_cur_freq_mhz",
                         device / "gt_cur_freq_mhz",
                         device / "gpu_cur_freq_mhz",
                         device / "current_freq_mhz"
                     }) {
                    if (const auto frequency = read_unsigned_file(candidate); frequency && *frequency > 0) {
                        metrics.frequency_mhz = static_cast<double>(*frequency);
                        break;
                    }
                }
            }
        }

        if (metrics.usage_percent && metrics.frequency_mhz) break;
        iterator.increment(error);
    }
    return metrics;
}

} // namespace

std::optional<SystemUsageSnapshot> SystemMonitorService::read(
    std::chrono::milliseconds cpu_sample_interval
) {
    const auto first_cpu = read_cpu_counters();
    const auto memory = read_memory_counters();
    if (!first_cpu || !memory) return std::nullopt;

    const auto gpu = read_gpu_metrics();
    std::optional<DrmEngineSnapshot> first_drm;
    std::optional<DrmEngineSnapshot> second_drm;
    std::chrono::steady_clock::time_point first_drm_time{};
    std::chrono::steady_clock::time_point second_drm_time{};
    if (!gpu.usage_percent) {
        first_drm = read_drm_engine_snapshot();
        first_drm_time = std::chrono::steady_clock::now();
    }
    if (cpu_sample_interval.count() > 0) std::this_thread::sleep_for(cpu_sample_interval);
    const auto second_cpu = read_cpu_counters();
    if (!gpu.usage_percent) {
        second_drm = read_drm_engine_snapshot();
        second_drm_time = std::chrono::steady_clock::now();
    }
    if (!second_cpu) return std::nullopt;

    SystemUsageSnapshot snapshot;
    const auto total_delta = second_cpu->total - first_cpu->total;
    const auto idle_delta = second_cpu->idle - first_cpu->idle;
    if (total_delta > 0) {
        snapshot.cpu_percent = std::clamp(
            100.0 * static_cast<double>(total_delta - std::min(idle_delta, total_delta)) /
                static_cast<double>(total_delta),
            0.0,
            100.0
        );
    }
    snapshot.cpu_frequency_mhz = read_cpu_frequency_mhz();

    snapshot.memory_total_kib = memory->total_kib;
    snapshot.memory_used_kib = memory->total_kib - std::min(
        memory->available_kib,
        memory->total_kib
    );
    snapshot.memory_percent = std::clamp(
        100.0 * static_cast<double>(snapshot.memory_used_kib) /
            static_cast<double>(snapshot.memory_total_kib),
        0.0,
        100.0
    );

    snapshot.swap_total_kib = memory->swap_total_kib;
    snapshot.swap_used_kib = memory->swap_total_kib - std::min(
        memory->swap_free_kib,
        memory->swap_total_kib
    );
    if (snapshot.swap_total_kib > 0) {
        snapshot.swap_percent = std::clamp(
            100.0 * static_cast<double>(snapshot.swap_used_kib) /
                static_cast<double>(snapshot.swap_total_kib),
            0.0,
            100.0
        );
    }

    snapshot.gpu_percent = gpu.usage_percent;
    if (!snapshot.gpu_percent && first_drm && second_drm) {
        snapshot.gpu_percent = calculate_drm_usage(
            *first_drm,
            *second_drm,
            second_drm_time - first_drm_time
        );
    }
    snapshot.gpu_frequency_mhz = gpu.frequency_mhz;
    return snapshot;
}

} // namespace realmheart::services
