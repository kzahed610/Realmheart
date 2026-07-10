#include "services/NightLight.hpp"

#include "core/Command.hpp"

#include <charconv>

namespace realmheart::services {
namespace {

constexpr int kNightTemperature = 4000;
constexpr int kDayTemperature = 6500;
constexpr int kEnabledThreshold = 6000;

std::optional<int> parse_temperature(const std::string& text) {
    int temperature = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, temperature);
    if (parsed.ec != std::errc{} || parsed.ptr != end || temperature < 1000 || temperature > 20000) {
        return std::nullopt;
    }
    return temperature;
}

} // namespace

std::optional<NightLightState> NightLight::read(const realmheart::core::CommandOptions& options) {
    if (!realmheart::core::command_exists("hyprctl")) return std::nullopt;
    const auto result = realmheart::core::run_capture(
        {"hyprctl", "hyprsunset", "temperature"},
        options
    );
    if (!result.succeeded() || result.truncated) return std::nullopt;

    const auto temperature = parse_temperature(result.output);
    if (!temperature) return std::nullopt;
    return NightLightState{*temperature < kEnabledThreshold, *temperature};
}

NightLightMutationResult NightLight::set_enabled(
    bool enabled,
    const realmheart::core::CommandOptions& options
) {
    NightLightMutationResult mutation;
    if (!read(options) && realmheart::core::command_exists("systemctl")) {
        static_cast<void>(realmheart::core::run_capture(
            {"systemctl", "--user", "start", "hyprsunset.service"},
            options
        ));
    }

    const int requested = enabled ? kNightTemperature : kDayTemperature;
    const auto write = realmheart::core::run_capture(
        {"hyprctl", "hyprsunset", "temperature", std::to_string(requested)},
        options
    );
    if (!write.succeeded()) {
        mutation.error = realmheart::core::command_failure_detail(write, "hyprsunset temperature failed");
        return mutation;
    }

    const auto readback = read(options);
    if (!readback) {
        mutation.error = "Night Light state unavailable after mutation";
        return mutation;
    }

    mutation.state = *readback;
    mutation.success = mutation.state.enabled == enabled && mutation.state.temperature == requested;
    if (!mutation.success) mutation.error = "Night Light readback did not match requested state";
    return mutation;
}

} // namespace realmheart::services
