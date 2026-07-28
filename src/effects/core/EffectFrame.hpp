#pragma once

namespace realmheart::effects {

enum class EffectId {
    None,
    FadeScale,
};

struct EffectFrame {
    double opacity = 1.0;
    double scale_x = 1.0;
    double scale_y = 1.0;
    double translate_x = 0.0;
    double translate_y = 0.0;
};

[[nodiscard]] EffectFrame sample_effect(
    EffectId effect,
    double progress
) noexcept;

} // namespace realmheart::effects
