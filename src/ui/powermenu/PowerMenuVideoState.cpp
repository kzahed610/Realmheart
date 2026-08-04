#include "ui/powermenu/PowerMenuVideoState.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::ui::powermenu {
namespace {

constexpr double kOpeningSeconds = 0.42;
constexpr double kClosingSeconds = 0.28;

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

double PowerMenuVideoState::opacity() const { return opacity_; }

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
