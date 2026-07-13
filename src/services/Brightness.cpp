#include "services/Brightness.hpp"

#include "core/Command.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace realmheart::services {
namespace {

std::optional<int> parse_integer(std::string_view value) {
    int parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return std::nullopt;
    return parsed;
}

std::optional<BrightnessState> parse_machine_line(std::string output) {
    output = realmheart::core::trim(std::move(output));
    if (output.empty()) return std::nullopt;

    // brightnessctl -m: device,class,current,percent,max
    std::vector<std::string> fields;
    std::stringstream stream(output);
    std::string field;
    while (std::getline(stream, field, ',')) fields.push_back(field);
    if (fields.size() < 5) return std::nullopt;

    const auto current = parse_integer(fields[2]);
    const auto maximum = parse_integer(fields[4]);
    std::string percent_text = fields[3];
    if (!percent_text.empty() && percent_text.back() == '%') percent_text.pop_back();
    const auto percent = parse_integer(percent_text);
    if (!current || !maximum || !percent || *maximum <= 0) return std::nullopt;

    return BrightnessState{*current, *maximum, static_cast<double>(*percent)};
}

} // namespace

std::optional<BrightnessState> Brightness::read(const realmheart::core::CommandOptions& options) {
    if (!realmheart::core::command_exists("brightnessctl")) return std::nullopt;

    const auto result = realmheart::core::run_capture({"brightnessctl", "-m", "info"}, options);
    if (result.succeeded() && !result.truncated) {
        if (const auto parsed = parse_machine_line(result.output)) return parsed;
    }

    // Compatibility fallback for older brightnessctl versions.
    const auto current = realmheart::core::run_capture({"brightnessctl", "g"}, options);
    const auto maximum = realmheart::core::run_capture({"brightnessctl", "m"}, options);
    if (!current.succeeded() || current.truncated || !maximum.succeeded() || maximum.truncated) {
        return std::nullopt;
    }

    const auto parsed_current = parse_integer(realmheart::core::trim(current.output));
    const auto parsed_maximum = parse_integer(realmheart::core::trim(maximum.output));
    if (!parsed_current || !parsed_maximum || *parsed_maximum <= 0) return std::nullopt;

    BrightnessState state;
    state.current = *parsed_current;
    state.maximum = *parsed_maximum;
    state.percent = (static_cast<double>(state.current) / state.maximum) * 100.0;
    return state;
}

BrightnessMutationResult Brightness::set_percent(
    int value,
    const realmheart::core::CommandOptions& options
) {
    BrightnessMutationResult mutation;
    if (!realmheart::core::command_exists("brightnessctl")) {
        mutation.error = "brightnessctl not found";
        return mutation;
    }

    const int requested = std::clamp(value, 0, 100);
    const auto result = realmheart::core::run_capture(
        {"brightnessctl", "-m", "set", std::to_string(requested) + "%"},
        options
    );
    if (!result.succeeded()) {
        mutation.error = realmheart::core::command_failure_detail(result, "brightnessctl set failed");
        return mutation;
    }

    // Modern brightnessctl reports the confirmed state in the mutation output,
    // avoiding two extra subprocesses on every slider commit.
    auto state = result.truncated ? std::nullopt : parse_machine_line(result.output);
    if (!state) state = read(options);
    if (!state) {
        mutation.error = "Brightness state unavailable after mutation";
        return mutation;
    }

    mutation.state = *state;
    mutation.success = std::abs(mutation.state.percent - requested) <= 1.0;
    if (!mutation.success) mutation.error = "Brightness readback did not match requested value";
    return mutation;
}

bool Brightness::set(int value, const realmheart::core::CommandOptions& options) {
    return set_percent(value, options).success;
}

} // namespace realmheart::services
