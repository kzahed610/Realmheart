#include "services/Audio.hpp"

#include "core/Command.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <sstream>
#include <string_view>

namespace realmheart::services {
namespace {

constexpr std::string_view kDefaultSink = "@DEFAULT_AUDIO_SINK@";

std::optional<AudioState> read_sink(
    std::string_view target,
    const realmheart::core::CommandOptions& options
) {
    const auto result = realmheart::core::run_capture(
        {"wpctl", "get-volume", std::string(target)},
        options
    );
    if (!result.succeeded() || result.truncated) return std::nullopt;

    std::istringstream fields(result.output);
    std::string label;
    double volume = 0.0;
    if (!(fields >> label >> volume) || label != "Volume:" || !std::isfinite(volume)) {
        return std::nullopt;
    }

    AudioState state;
    state.raw_status = result.output;
    state.volume = volume;
    state.muted = result.output.find("[MUTED]") != std::string::npos;
    return state;
}

std::optional<std::string> sink_id_from_status(
    const realmheart::core::CommandOptions& options
) {
    auto status = realmheart::core::run_capture(
        {"wpctl", "status", "-n"},
        options
    );
    if (!status.succeeded()) {
        status = realmheart::core::run_capture({"wpctl", "status"}, options);
    }
    if (!status.succeeded() || status.truncated || status.output.empty()) {
        return std::nullopt;
    }

    bool in_sinks = false;
    std::optional<std::string> first_sink;
    std::optional<std::string> preferred_hardware_sink;
    std::stringstream lines(status.output);
    std::string line;
    while (std::getline(lines, line)) {
        if (!in_sinks) {
            if (line.find("Sinks:") != std::string::npos) in_sinks = true;
            continue;
        }

        if (line.find("Sources:") != std::string::npos ||
            line.find("Filters:") != std::string::npos ||
            line.find("Streams:") != std::string::npos ||
            line.find("Settings") != std::string::npos ||
            line.find("Video") != std::string::npos) {
            break;
        }

        const auto digit = line.find_first_of("0123456789");
        if (digit == std::string::npos) continue;
        const auto end = line.find_first_not_of("0123456789", digit);
        if (end == std::string::npos || line[end] != '.') continue;

        const std::string id = line.substr(digit, end - digit);
        int parsed = 0;
        const auto [parsed_end, error] = std::from_chars(
            id.data(), id.data() + id.size(), parsed
        );
        if (error != std::errc{} || parsed_end != id.data() + id.size() || parsed <= 0) {
            continue;
        }

        if (!first_sink) first_sink = id;
        if (line.find('*') != std::string::npos) return id;
        if (!preferred_hardware_sink &&
            (line.find("Built-in") != std::string::npos ||
             line.find("Speaker") != std::string::npos ||
             line.find("Analog Stereo") != std::string::npos)) {
            preferred_hardware_sink = id;
        }
    }
    return preferred_hardware_sink ? preferred_hardware_sink : first_sink;
}

std::optional<std::string> writable_sink_target(
    const realmheart::core::CommandOptions& options
) {
    if (read_sink(kDefaultSink, options)) return std::string(kDefaultSink);
    return sink_id_from_status(options);
}

} // namespace

std::optional<AudioState> Audio::read_default_sink(
    const realmheart::core::CommandOptions& options
) {
    if (!realmheart::core::command_exists("wpctl")) return std::nullopt;
    if (const auto direct = read_sink(kDefaultSink, options)) return direct;
    const auto fallback = sink_id_from_status(options);
    return fallback ? read_sink(*fallback, options) : std::nullopt;
}

AudioMutationResult Audio::set_default_sink_volume(
    double volume,
    const realmheart::core::CommandOptions& options
) {
    AudioMutationResult mutation;
    if (!realmheart::core::command_exists("wpctl")) {
        mutation.error = "wpctl not found";
        return mutation;
    }

    const auto target = writable_sink_target(options);
    if (!target) {
        mutation.error = "No usable audio sink found";
        return mutation;
    }

    const double requested = std::clamp(volume, 0.0, 1.0);
    const auto write = realmheart::core::run_capture(
        {"wpctl", "set-volume", *target, std::to_string(requested)},
        options
    );
    if (!write.succeeded()) {
        mutation.error = realmheart::core::command_failure_detail(write, "wpctl set-volume failed");
        return mutation;
    }

    const auto readback = read_sink(*target, options);
    if (!readback) {
        mutation.error = "Audio state unavailable after mutation";
        return mutation;
    }

    mutation.state = *readback;
    mutation.success = std::abs(mutation.state.volume - requested) <= 0.005;
    if (!mutation.success) mutation.error = "Audio readback did not match requested volume";
    return mutation;
}

} // namespace realmheart::services
