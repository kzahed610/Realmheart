#include "services/Audio.hpp"

#include "core/Command.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace realmheart::services {

std::optional<AudioState> Audio::read_default_sink(
    const realmheart::core::CommandOptions& options
) {
    if (!realmheart::core::command_exists("wpctl")) return std::nullopt;
    const auto result = realmheart::core::run_capture(
        {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"},
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

AudioMutationResult Audio::set_default_sink_volume(
    double volume,
    const realmheart::core::CommandOptions& options
) {
    AudioMutationResult mutation;
    if (!realmheart::core::command_exists("wpctl")) {
        mutation.error = "wpctl not found";
        return mutation;
    }

    const double requested = std::clamp(volume, 0.0, 1.5);
    const auto write = realmheart::core::run_capture(
        {"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", std::to_string(requested)},
        options
    );
    if (!write.succeeded()) {
        mutation.error = realmheart::core::command_failure_detail(write, "wpctl set-volume failed");
        return mutation;
    }

    const auto readback = read_default_sink(options);
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
