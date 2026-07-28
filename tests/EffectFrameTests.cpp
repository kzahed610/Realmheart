#include "effects/core/EffectFrame.hpp"

#include <cassert>
#include <cmath>
#include <limits>

namespace {

bool near(double left, double right, double epsilon = 0.000001) {
    return std::abs(left - right) <= epsilon;
}

} // namespace

int main() {
    using realmheart::effects::EffectId;
    using realmheart::effects::sample_effect;

    const auto none_hidden = sample_effect(EffectId::None, 0.0);
    assert(near(none_hidden.opacity, 1.0));
    assert(near(none_hidden.scale_x, 1.0));
    assert(near(none_hidden.scale_y, 1.0));

    const auto hidden = sample_effect(EffectId::FadeScale, 0.0);
    assert(near(hidden.opacity, 0.0));
    assert(near(hidden.scale_x, 0.965));
    assert(near(hidden.scale_y, 0.965));

    const auto visible = sample_effect(EffectId::FadeScale, 1.0);
    assert(near(visible.opacity, 1.0));
    assert(near(visible.scale_x, 1.0));
    assert(near(visible.scale_y, 1.0));

    const auto before = sample_effect(EffectId::FadeScale, 0.35);
    const auto after = sample_effect(EffectId::FadeScale, 0.70);
    assert(after.opacity > before.opacity);
    assert(after.scale_x > before.scale_x);
    assert(after.scale_y > before.scale_y);

    const auto clamped_low = sample_effect(EffectId::FadeScale, -4.0);
    assert(near(clamped_low.opacity, hidden.opacity));
    assert(near(clamped_low.scale_x, hidden.scale_x));

    const auto clamped_high = sample_effect(EffectId::FadeScale, 8.0);
    assert(near(clamped_high.opacity, visible.opacity));
    assert(near(clamped_high.scale_x, visible.scale_x));

    const auto invalid = sample_effect(
        EffectId::FadeScale,
        std::numeric_limits<double>::quiet_NaN()
    );
    assert(near(invalid.opacity, hidden.opacity));
    assert(near(invalid.scale_x, hidden.scale_x));

    return 0;
}
