#include "services/Audio.hpp"

#include "core/Command.hpp"

namespace realmheart::services {

std::optional<AudioState> Audio::read_default_sink() {
    if (!realmheart::core::command_exists("wpctl")) return std::nullopt;
    auto result = realmheart::core::run_capture({"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"});
    if (!result.succeeded() || result.truncated) return std::nullopt;
    return AudioState{result.output};
}

} // namespace realmheart::services
