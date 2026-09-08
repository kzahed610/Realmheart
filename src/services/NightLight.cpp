#include "services/NightLight.hpp"

#include "core/Command.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace realmheart::services {
namespace {

using namespace std::chrono_literals;
constexpr std::size_t kMaximumStateBytes = 64;

std::filesystem::path state_path() {
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        runtime != nullptr && *runtime != '\0') {
        return std::filesystem::path(runtime) / "realmheart-night-light.state";
    }
    return std::filesystem::path("/tmp") /
        ("realmheart-" + std::to_string(::getuid())) / "night-light.state";
}

realmheart::core::CommandOptions operation_options(
    const realmheart::core::CommandOptions& input
) {
    auto options = input;
    if (options.deadline == 1500ms) options.deadline = 4s;
    const auto requested_deadline = std::chrono::steady_clock::now() + options.deadline;
    if (!options.deadline_at || requested_deadline < *options.deadline_at) {
        options.deadline_at = requested_deadline;
    }
    return options;
}

NightLightState default_state() {
    return NightLightState{false, NightLight::kDefaultTemperature};
}

bool private_regular_file(const std::filesystem::path& path, std::size_t* size = nullptr) {
    struct stat metadata {};
    if (::lstat(path.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode)) return false;
    if (metadata.st_uid != ::getuid() || (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0) return false;
    if (size != nullptr) *size = static_cast<std::size_t>(metadata.st_size);
    return true;
}

NightLightState load_state() {
    std::size_t size = 0;
    if (!private_regular_file(state_path(), &size) || size > kMaximumStateBytes) return default_state();
    std::ifstream input(state_path(), std::ios::binary);
    int enabled = 0;
    int temperature = NightLight::kDefaultTemperature;
    if (!(input >> enabled >> temperature)) return default_state();
    if (enabled != 0 && enabled != 1) return default_state();
    if (temperature < NightLight::kMinimumTemperature ||
        temperature > NightLight::kMaximumTemperature) return default_state();
    return NightLightState{enabled == 1, temperature};
}

std::optional<int> live_temperature(const realmheart::core::CommandOptions& options) {
    const auto result = realmheart::core::run_capture(
        {"hyprctl", "hyprsunset", "temperature"},
        options
    );
    if (!result.succeeded() || result.truncated || result.output.empty()) return std::nullopt;

    const auto value = realmheart::core::trim(result.output);
    int temperature = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), temperature
    );
    if (error != std::errc{} || end != value.data() + value.size() ||
        temperature < NightLight::kMinimumTemperature ||
        temperature > NightLight::kMaximumTemperature) {
        return std::nullopt;
    }
    return temperature;
}

bool save_state(const NightLightState& state) {
    const auto path = state_path();
    std::error_code error;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return false;
        if (::chmod(parent.c_str(), 0700) != 0) return false;
        struct stat parent_metadata {};
        if (::stat(parent.c_str(), &parent_metadata) != 0 ||
            parent_metadata.st_uid != ::getuid() || (parent_metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            return false;
        }
    }

    const std::string serialized = std::to_string(state.enabled ? 1 : 0) +
        " " + std::to_string(state.temperature) + "\n";
    const auto temporary = std::filesystem::path(path.string() + ".tmp." + std::to_string(::getpid()));
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (descriptor < 0) return false;
    const ssize_t written = ::write(descriptor, serialized.data(), serialized.size());
    const bool complete = written == static_cast<ssize_t>(serialized.size());
    static_cast<void>(::fchmod(descriptor, 0600));
    static_cast<void>(::fsync(descriptor));
    ::close(descriptor);
    if (!complete || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    return true;
}

std::optional<realmheart::core::BackgroundProcess>& fallback_process() {
    static auto* process = new std::optional<realmheart::core::BackgroundProcess>();
    return *process;
}

std::mutex& fallback_mutex() {
    static auto* mutex = new std::mutex();
    return *mutex;
}

realmheart::core::CommandResult issue_ipc(
    bool enabled,
    int temperature,
    const realmheart::core::CommandOptions& options
) {
    return enabled
        ? realmheart::core::run_capture(
            {"hyprctl", "hyprsunset", "temperature", std::to_string(temperature)}, options
        )
        : realmheart::core::run_capture(
            {"hyprctl", "hyprsunset", "identity"}, options
        );
}

realmheart::core::CommandResult issue_with_startup(
    bool enabled,
    int temperature,
    const realmheart::core::CommandOptions& input
) {
    const auto options = operation_options(input);
    auto result = issue_ipc(enabled, temperature, options);
    if (result.succeeded()) return result;

    std::optional<realmheart::core::BackgroundProcess> started_fallback;
    {
        std::lock_guard lock(fallback_mutex());
        if (fallback_process().has_value() && fallback_process()->valid()) {
            started_fallback.emplace(std::move(*fallback_process()));
            fallback_process().reset();
        }
    }

    bool launched = false;
    if (!started_fallback && realmheart::core::command_exists("systemctl")) {
        const auto started = realmheart::core::run_capture(
            {"systemctl", "--user", "start", "hyprsunset.service"}, options
        );
        launched = started.succeeded();
    }
    if (!launched && !started_fallback) {
        started_fallback = realmheart::core::run_background_tracked({"hyprsunset"});
        launched = started_fallback.has_value();
    }
    if (!launched) return result;

    for (int attempt = 0; attempt < 8; ++attempt) {
        if (options.cancelled && options.cancelled()) break;
        if (options.deadline_at && std::chrono::steady_clock::now() >= *options.deadline_at) break;
        std::this_thread::sleep_for(45ms);
        result = issue_ipc(enabled, temperature, options);
        if (result.succeeded()) {
            if (started_fallback) {
                std::lock_guard lock(fallback_mutex());
                fallback_process() = std::move(*started_fallback);
            }
            return result;
        }
    }

    if (started_fallback) static_cast<void>(started_fallback->stop(100ms));
    return result;
}

NightLightMutationResult mutate(
    bool enabled,
    int temperature,
    const realmheart::core::CommandOptions& options
) {
    NightLightMutationResult mutation;
    if (!realmheart::core::command_exists("hyprsunset")) {
        mutation.error = "hyprsunset is not installed";
        return mutation;
    }
    if (!realmheart::core::command_exists("hyprctl")) {
        mutation.error = "hyprctl is not installed";
        return mutation;
    }

    temperature = std::clamp(
        temperature,
        NightLight::kMinimumTemperature,
        NightLight::kMaximumTemperature
    );
    const auto write = issue_with_startup(enabled, temperature, options);
    if (!write.succeeded()) {
        mutation.error = realmheart::core::command_failure_detail(
            write,
            enabled ? "Unable to set Night Light temperature" : "Unable to disable Night Light"
        );
        return mutation;
    }

    mutation.state = NightLightState{enabled, temperature};
    if (!save_state(mutation.state)) {
        mutation.partial = true;
        mutation.error = "Night Light IPC applied, but state persistence failed";
        return mutation;
    }
    mutation.success = true;
    return mutation;
}

} // namespace

std::optional<NightLightState> NightLight::read(
    const realmheart::core::CommandOptions& input
) {
    if (!realmheart::core::command_exists("hyprctl") ||
        !realmheart::core::command_exists("hyprsunset")) return std::nullopt;

    const auto options = operation_options(input);
    const auto live_session = realmheart::core::run_capture(
        {"hyprctl", "monitors", "-j"}, options
    );
    if (!live_session.succeeded() || live_session.truncated || live_session.output.empty()) {
        return std::nullopt;
    }
    const auto daemon_temperature = live_temperature(options);
    if (!daemon_temperature) return std::nullopt;

    const auto state = load_state();
    if (*daemon_temperature != state.temperature) return std::nullopt;
    return state;
}

bool NightLight::recovery_available() {
    return realmheart::core::command_exists("hyprctl") &&
        realmheart::core::command_exists("hyprsunset");
}

NightLightMutationResult NightLight::set_enabled(
    bool enabled,
    const realmheart::core::CommandOptions& options
) {
    const auto remembered = load_state();
    return mutate(enabled, remembered.temperature, options);
}

NightLightMutationResult NightLight::set_enabled(
    bool enabled,
    int temperature,
    const realmheart::core::CommandOptions& options
) {
    return mutate(enabled, temperature, options);
}

NightLightMutationResult NightLight::set_temperature(
    int temperature,
    const realmheart::core::CommandOptions& options
) {
    return mutate(true, temperature, options);
}

int NightLight::strength_to_temperature(int strength_percent) {
    const int strength = std::clamp(strength_percent, 0, 100);
    const int span = kMaximumTemperature - kMinimumTemperature;
    return kMaximumTemperature - static_cast<int>(
        std::lround(static_cast<double>(span) * strength / 100.0)
    );
}

int NightLight::temperature_to_strength(int temperature) {
    const int clamped = std::clamp(
        temperature,
        kMinimumTemperature,
        kMaximumTemperature
    );
    const int span = kMaximumTemperature - kMinimumTemperature;
    return static_cast<int>(std::lround(
        static_cast<double>(kMaximumTemperature - clamped) * 100.0 / span
    ));
}

} // namespace realmheart::services
