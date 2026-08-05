#include "ui/powermenu/PowerMenuVideoState.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::ui::powermenu {
namespace {

constexpr double kOpeningSeconds = 1.75;
constexpr double kClosingSeconds = 1.05;

double smoothstep(double value) {
    const double t = std::clamp(value, 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

} // namespace

void PowerMenuVideoState::present() {
    if (phase_ == PowerMenuVideoPhase::Hidden ||
        phase_ == PowerMenuVideoPhase::Closing) {
        phase_ = PowerMenuVideoPhase::Opening;
    }
    sample();
}

void PowerMenuVideoState::dismiss() {
    if (phase_ != PowerMenuVideoPhase::Hidden) {
        phase_ = PowerMenuVideoPhase::Closing;
    }
    sample();
}

void PowerMenuVideoState::hide_immediately() {
    phase_ = PowerMenuVideoPhase::Hidden;
    progress_ = 0.0;
    sample();
}

void PowerMenuVideoState::advance(double delta_seconds) {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0) return;

    const double dt = std::min(delta_seconds, 1.0);
    if (phase_ == PowerMenuVideoPhase::Opening) {
        progress_ = std::min(1.0, progress_ + (dt / kOpeningSeconds));
        if (progress_ >= 1.0) phase_ = PowerMenuVideoPhase::Visible;
    } else if (phase_ == PowerMenuVideoPhase::Closing) {
        progress_ = std::max(0.0, progress_ - (dt / kClosingSeconds));
        if (progress_ <= 0.0) phase_ = PowerMenuVideoPhase::Hidden;
    }
    sample();
}

PowerMenuVideoPhase PowerMenuVideoState::phase() const { return phase_; }

double PowerMenuVideoState::progress() const { return progress_; }

double PowerMenuVideoState::opacity() const { return opacity_; }

double PowerMenuVideoState::controls_opacity() const {
    // Keep the controls absent while the travelling front is still near the
    // taskbar. They fade in only once the rupture has reached the central menu
    // stack, which ties the UI reveal more tightly to the screen-space wave.
    const double delayed = (progress_ - 0.44) / 0.18;
    return smoothstep(delayed);
}

bool PowerMenuVideoState::media_required() const {
    return phase_ != PowerMenuVideoPhase::Hidden;
}

bool PowerMenuVideoState::needs_frame() const {
    return phase_ == PowerMenuVideoPhase::Opening ||
        phase_ == PowerMenuVideoPhase::Closing;
}

void PowerMenuVideoState::sample() {
    opacity_ = smoothstep(progress_);
}

} // namespace realmheart::ui::powermenu
