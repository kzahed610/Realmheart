#include "ui/lockscreen/ScalesStateMachine.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::ui::lockscreen {

// Duration of the scale-field emergence.
constexpr double kFormingSeconds = 2.50;
// Duration of the wrong-password red flash, after which the field returns
// to Idle for another attempt.
constexpr double kFailingSeconds = 2.00;
// Duration of the unlock erosion (scales dissolve one-by-one, sweeping
// outward from the center).
constexpr double kClosingSeconds = 0.80;

void ScalesStateMachine::present() noexcept {
    phase_ = ScalesPhase::Forming;
    progress_ = 0.0;
}

void ScalesStateMachine::dismiss() noexcept {
    if (phase_ == ScalesPhase::Hidden) return;
    phase_ = ScalesPhase::Closing;
    progress_ = 1.0;
}

void ScalesStateMachine::fail() noexcept {
    if (phase_ == ScalesPhase::Forming || phase_ == ScalesPhase::Idle) {
        phase_ = ScalesPhase::Failing;
        progress_ = 0.0;
    }
}

void ScalesStateMachine::hide_immediately() noexcept {
    phase_ = ScalesPhase::Hidden;
    progress_ = 0.0;
}

void ScalesStateMachine::advance(double delta_seconds) noexcept {
    if (!std::isfinite(delta_seconds)) return;
    const double clamped = std::clamp(delta_seconds, 0.0, 1.0);

    switch (phase_) {
    case ScalesPhase::Forming:
        progress_ += clamped / kFormingSeconds;
        if (progress_ >= 1.0) {
            progress_ = 0.0;
            phase_ = ScalesPhase::Idle;
        }
        break;
    case ScalesPhase::Failing:
        progress_ += clamped / kFailingSeconds;
        if (progress_ >= 1.0) {
            progress_ = 0.0;
            phase_ = ScalesPhase::Idle;
        }
        break;
    case ScalesPhase::Closing:
        progress_ -= clamped / kClosingSeconds;
        if (progress_ <= 0.0) {
            progress_ = 0.0;
            phase_ = ScalesPhase::Hidden;
        }
        break;
    default:
        break;
    }
}

bool ScalesStateMachine::needs_frame() const noexcept {
    return phase_ == ScalesPhase::Forming ||
        phase_ == ScalesPhase::Failing ||
        phase_ == ScalesPhase::Closing;
}

} // namespace realmheart::ui::lockscreen
