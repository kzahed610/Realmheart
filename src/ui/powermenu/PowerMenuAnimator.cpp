#include "ui/powermenu/PowerMenuAnimator.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace realmheart::ui::powermenu {
namespace {

constexpr double kOpeningSeconds = 0.42;
constexpr double kClosingSeconds = 0.28;
constexpr double kDoubleBlinkGap = 0.11;
constexpr double kTau = 2.0 * std::numbers::pi;

std::uint64_t splitmix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

double unit_random(std::uint64_t value) {
    return static_cast<double>(splitmix64(value) >> 11U) *
        (1.0 / 9007199254740992.0);
}

double smoothstep(double value) {
    const double t = std::clamp(value, 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

double bounded_wave(double elapsed, double frequency, double phase) {
    return std::sin(kTau * ((elapsed * frequency) + phase));
}

} // namespace

PowerMenuAnimator::PowerMenuAnimator(const PowerMenuRig& rig, std::uint64_t seed)
    : rig_(rig), seed_(seed) {
    frame_.layers.resize(rig_.layers.size());
    sample_frame();
}

void PowerMenuAnimator::open() {
    if (phase_ == PowerMenuScenePhase::Hidden ||
        phase_ == PowerMenuScenePhase::Closing) {
        phase_ = PowerMenuScenePhase::Opening;
    }
    sample_frame();
}

void PowerMenuAnimator::close() {
    if (phase_ == PowerMenuScenePhase::Confirming) {
        phase_ = PowerMenuScenePhase::Idle;
    } else if (phase_ != PowerMenuScenePhase::Hidden) {
        phase_ = PowerMenuScenePhase::Closing;
    }
    sample_frame();
}

void PowerMenuAnimator::set_confirming(bool confirming) {
    if (confirming && phase_ == PowerMenuScenePhase::Idle) {
        phase_ = PowerMenuScenePhase::Confirming;
    } else if (!confirming && phase_ == PowerMenuScenePhase::Confirming) {
        phase_ = PowerMenuScenePhase::Idle;
    }
    sample_frame();
}

void PowerMenuAnimator::advance(double delta_seconds) {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0 ||
        phase_ == PowerMenuScenePhase::Hidden) return;
    const double dt = std::min(delta_seconds, 1.0);
    visible_elapsed_ += dt;
    if (phase_ == PowerMenuScenePhase::Opening) {
        lifecycle_progress_ = std::min(
            1.0, lifecycle_progress_ + (dt / kOpeningSeconds)
        );
        if (lifecycle_progress_ >= 1.0) phase_ = PowerMenuScenePhase::Idle;
    } else if (phase_ == PowerMenuScenePhase::Closing) {
        lifecycle_progress_ = std::max(
            0.0, lifecycle_progress_ - (dt / kClosingSeconds)
        );
        if (lifecycle_progress_ <= 0.0) phase_ = PowerMenuScenePhase::Hidden;
    }
    sample_frame();
}

PowerMenuScenePhase PowerMenuAnimator::phase() const { return phase_; }

bool PowerMenuAnimator::needs_frame() const {
    return phase_ != PowerMenuScenePhase::Hidden;
}

const PowerMenuFrame& PowerMenuAnimator::frame() const { return frame_; }

double PowerMenuAnimator::blink_interval(std::uint64_t index) const {
    const double minimum = rig_.blink.minimum_interval_seconds;
    const double maximum = rig_.blink.maximum_interval_seconds;
    return minimum + ((maximum - minimum) * unit_random(seed_ ^ (index * 2U)));
}

bool PowerMenuAnimator::double_blink(std::uint64_t index) const {
    return unit_random(seed_ ^ (index * 2U) ^ 0xa5a5a5a5ULL) <
        rig_.blink.double_blink_chance;
}

PowerMenuBlinkState PowerMenuAnimator::sample_blink(double elapsed) const {
    if (phase_ == PowerMenuScenePhase::Closing ||
        phase_ == PowerMenuScenePhase::Hidden ||
        rig_.blink.minimum_interval_seconds <= 0.0) {
        return PowerMenuBlinkState::Open;
    }

    const double half_seconds = rig_.blink.duration_seconds * (2.0 / 7.0);
    const double closed_seconds = rig_.blink.duration_seconds * (3.0 / 7.0);
    const double opening_half_seconds = rig_.blink.duration_seconds -
        half_seconds - closed_seconds;
    double deadline = blink_interval(0U);
    for (std::uint64_t cycle = 0; cycle < 4096U && elapsed >= deadline; ++cycle) {
        const double local = elapsed - deadline;
        const auto state_at = [half_seconds, closed_seconds, opening_half_seconds](
                                  double blink_time) {
            const double duration = half_seconds + closed_seconds + opening_half_seconds;
            if (blink_time < 0.0 || blink_time >= duration) {
                return PowerMenuBlinkState::Open;
            }
            if (blink_time < half_seconds) return PowerMenuBlinkState::Half;
            if (blink_time < half_seconds + closed_seconds) {
                return PowerMenuBlinkState::Closed;
            }
            return PowerMenuBlinkState::Half;
        };
        const PowerMenuBlinkState first = state_at(local);
        if (first != PowerMenuBlinkState::Open) return first;
        const bool doubles = double_blink(cycle);
        if (doubles) {
            const PowerMenuBlinkState second = state_at(
                local - rig_.blink.duration_seconds - kDoubleBlinkGap
            );
            if (second != PowerMenuBlinkState::Open) return second;
        }
        deadline += rig_.blink.duration_seconds +
            (doubles ? rig_.blink.duration_seconds + kDoubleBlinkGap : 0.0) +
            blink_interval(cycle + 1U);
    }
    return PowerMenuBlinkState::Open;
}

void PowerMenuAnimator::sample_frame() {
    const double eased = smoothstep(lifecycle_progress_);
    frame_.scene_opacity = eased;
    frame_.scene_scale = 0.985 + (0.015 * eased);
    frame_.blink = sample_blink(visible_elapsed_);
    frame_.iris_glow = 0.0;
    frame_.rune_glow = 0.0;
    if (frame_.layers.size() != rig_.layers.size()) {
        frame_.layers.resize(rig_.layers.size());
    }

    for (std::size_t index = 0; index < rig_.layers.size(); ++index) {
        const auto& layer = rig_.layers[index];
        const auto& animation = layer.animation;
        PowerMenuLayerMotionSample sample;
        switch (animation.type) {
        case PowerMenuAnimationType::Static:
        case PowerMenuAnimationType::BlinkPatch:
            break;
        case PowerMenuAnimationType::Drift: {
            const double wave = bounded_wave(
                visible_elapsed_, animation.frequency, animation.phase
            );
            const double cross = bounded_wave(
                visible_elapsed_, animation.frequency * 0.73, animation.phase + 0.19
            );
            sample.translation_x = animation.translation.x * wave;
            sample.translation_y = animation.translation.y * cross;
            sample.opacity = std::clamp(
                1.0 - (animation.opacity_amplitude * (0.5 + (0.5 * wave))),
                0.0, 1.0
            );
            break;
        }
        case PowerMenuAnimationType::FlowDrift: {
            const double wave = bounded_wave(
                visible_elapsed_, animation.frequency, animation.phase
            );
            const double cross = bounded_wave(
                visible_elapsed_, animation.frequency * 0.79, animation.phase + 0.23
            );
            sample.translation_x = animation.translation.x * wave;
            sample.translation_y = animation.translation.y * cross;
            sample.scale = 1.0 + (animation.scale_amplitude * cross);
            sample.opacity = std::clamp(1.0 -
                (animation.opacity_amplitude * (0.5 + (0.5 * wave))), 0.0, 1.0);
            sample.flow_displacement = wave;
            break;
        }
        case PowerMenuAnimationType::Spring: {
            const double wave = bounded_wave(
                visible_elapsed_, animation.frequency, animation.phase
            );
            const double response = wave * (1.0 - (0.15 * animation.damping));
            sample.translation_x = animation.translation.x * response;
            sample.translation_y = animation.translation.y * response;
            sample.rotation_degrees = animation.rotation_degrees * response;
            break;
        }
        case PowerMenuAnimationType::MeshFlow: {
            const double flow_wave = bounded_wave(
                visible_elapsed_, animation.flow.frequency, animation.flow.phase
            );
            const double macro_wave = bounded_wave(
                visible_elapsed_, animation.flow.frequency * 0.61,
                animation.flow.phase + 0.17
            );
            sample.macro_displacement = animation.mesh.amplitude * macro_wave;
            sample.flow_displacement = animation.flow.amplitude * flow_wave;
            break;
        }
        case PowerMenuAnimationType::GlowMask: {
            const double phase_offset = animation.tint_role == "iris-gold" ? 0.13 : 0.57;
            const double wave = 0.68 * bounded_wave(
                visible_elapsed_, animation.frequency, phase_offset
            ) + 0.32 * bounded_wave(
                visible_elapsed_, animation.frequency * 0.44375, phase_offset + 0.31
            );
            const double normalized = std::clamp(0.5 + (0.5 * wave), 0.0, 1.0);
            sample.opacity = animation.idle_minimum +
                ((animation.idle_maximum - animation.idle_minimum) * normalized);
            if (animation.tint_role == "iris-gold") {
                const double blink_factor = frame_.blink == PowerMenuBlinkState::Open
                    ? 1.0 : (frame_.blink == PowerMenuBlinkState::Half ? 0.30 : 0.0);
                sample.opacity *= blink_factor;
                frame_.iris_glow = sample.opacity;
            } else if (animation.tint_role == "mana-gold") {
                if (phase_ == PowerMenuScenePhase::Confirming) {
                    sample.opacity = std::min(1.0, sample.opacity + 0.20);
                }
                frame_.rune_glow = sample.opacity;
            }
            break;
        }
        }
        frame_.layers[index] = sample;
    }
}

} // namespace realmheart::ui::powermenu
