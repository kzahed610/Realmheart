#pragma once

namespace realmheart::effects {

struct ShaderPlaybackFrame {
    float progress = 0.0F;
    float reverse = 0.0F;
};

[[nodiscard]] ShaderPlaybackFrame sample_shader_playback(
    double timeline_progress,
    bool opening
) noexcept;

} // namespace realmheart::effects
