#include "services/NightLight.hpp"

#include "core/Command.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>

namespace realmheart::services {
namespace {

using namespace std::chrono_literals;

std::filesystem::path state_path() {
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        runtime != nullptr && *runtime != '\0') {
        return std::filesystem::path(runtime) / "realmheart-night-light.state";
    }
    return std::filesystem::temp_directory_path() /
        ("realmheart-night-light-" + std::to_string(::getuid()) + ".state");
}

NightLightState default_state() {
    return NightLightState{false, NightLight::kDefaultTemperature};
}

NightLightState load_state() {
    std::ifstream input(state_path());
    int enabled = 0;
    int temperature = NightLight::kDefaultTemperature;
    if (!(input >> enabled >> temperature)) return default_state();
    temperature = std::clamp(
        temperature,
        NightLight::kMinimumTemperature,
        NightLight::kMaximumTemperature
    );
    return NightLightState{enabled != 0, temperature};
}

void save_state(const NightLightState& state) {
    const auto path = state_path();
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }

    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        output << (state.enabled ? 1 : 0) << ' ' << state.temperature << '\n';
        if (!output) return;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
}

realmheart::core::CommandResult issue_ipc(
    bool enabled,
    int temperature,
    const realmheart::core::CommandOptions& options
) {
    return enabled
        ? realmheart::core::run_capture(
            {"hyprctl", "hyprsunset", "temperature", std::to_string(temperature)},
            options
        )
        : realmheart::core::run_capture(
            {"hyprctl", "hyprsunset", "identity"},
            options
        );
}

realmheart::core::CommandResult issue_with_startup(
    bool enabled,
    int temperature,
    const realmheart::core::CommandOptions& options
) {
    auto result = issue_ipc(enabled, temperature, options);
    if (result.succeeded()) return result;

    bool launched = false;
    if (realmheart::core::command_exists("systemctl")) {
        const auto started = realmheart::core::run_capture(
            {"systemctl", "--user", "start", "hyprsunset.service"},
            options
        );
        launched = started.succeeded();
    }
    if (!launched) {
        launched = realmheart::core::run_background({"hyprsunset"});
    }
    if (!launched) return result;

    // hyprsunset needs a brief moment to register its Hyprland IPC endpoint.
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::this_thread::sleep_for(45ms);
        result = issue_ipc(enabled, temperature, options);
        if (result.succeeded()) return result;
    }
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

    mutation.success = true;
    mutation.state = NightLightState{enabled, temperature};
    save_state(mutation.state);
    return mutation;
}

} // namespace

std::optional<NightLightState> NightLight::read(
    const realmheart::core::CommandOptions&
) {
    if (!realmheart::core::command_exists("hyprctl") ||
        !realmheart::core::command_exists("hyprsunset")) {
        return std::nullopt;
    }

    // hyprsunset exposes setters through IPC but does not expose a reliable
    // getter for the live temperature. Realmheart therefore remembers the last
    // state it successfully applied instead of issuing unsupported "get" calls.
    return load_state();
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
