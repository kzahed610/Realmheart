#include "effects/core/ShaderPlayback.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::effects {
namespace {

float clamp_unit(double value) noexcept {
    if (!std::isfinite(value)) return 0.0F;
    return static_cast<float>(std::clamp(value, 0.0, 1.0));
}

} // namespace

ShaderPlaybackFrame sample_shader_playback(
    double timeline_progress,
    bool opening
) noexcept {
    const float timeline = clamp_unit(timeline_progress);
    return {
        .progress = opening ? timeline : 1.0F - timeline,
        .reverse = opening ? 1.0F : 0.0F,
    };
}

} // namespace realmheart::effects
