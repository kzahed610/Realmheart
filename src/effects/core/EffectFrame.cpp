#include "effects/core/EffectFrame.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::effects {
namespace {

double clamp_unit(double value) noexcept {
    if (!std::isfinite(value)) return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

double ease_out_cubic(double value) noexcept {
    const double progress = clamp_unit(value);
    const double inverse = 1.0 - progress;
    return 1.0 - (inverse * inverse * inverse);
}

} // namespace

EffectFrame sample_effect(EffectId effect, double progress) noexcept {
    const double normalized = clamp_unit(progress);

    switch (effect) {
    case EffectId::None:
        return {};
    case EffectId::FadeScale: {
        constexpr double kInitialScale = 0.965;
        const double eased = ease_out_cubic(normalized);
        const double scale = kInitialScale + ((1.0 - kInitialScale) * eased);
        return {
            .opacity = normalized,
            .scale_x = scale,
            .scale_y = scale,
            .translate_x = 0.0,
            .translate_y = 0.0,
        };
    }
    case EffectId::Void:
        // Shader-backed effects do not modify the GTK snapshot frame.
        return {};
    }

    return {};
}

} // namespace realmheart::effects
