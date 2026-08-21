#include "ui/lockscreen/LockStateMachine.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::ui::lockscreen {

// Duration of the horn emergence (pop-in) stage.
constexpr double kOpeningSeconds = 0.70;
// Duration of the crystal fracture + 180° rotation + outward drift stage.
// Slower than the spec so the flip reads cleanly; the halves then hold in the
// Typing state (no per-frame ticking needed).
constexpr double kSplittingSeconds = 0.90;
// Duration of the crystal fade-out / scale-down stage.
constexpr double kClosingSeconds = 0.30;

void LockStateMachine::present() noexcept {
    phase_ = LockPhase::Opening;
    progress_ = 0.0;
}

void LockStateMachine::dismiss() noexcept {
    if (phase_ == LockPhase::Hidden) return;
    phase_ = LockPhase::Closing;
    progress_ = 1.0;
}

void LockStateMachine::hide_immediately() noexcept {
    phase_ = LockPhase::Hidden;
    progress_ = 0.0;
}

void LockStateMachine::advance(double delta_seconds) noexcept {
    if (!std::isfinite(delta_seconds)) return;
    const double clamped = std::clamp(delta_seconds, 0.0, 1.0);

    switch (phase_) {
    case LockPhase::Opening:
        progress_ += clamped / kOpeningSeconds;
        if (progress_ >= 1.0) {
            progress_ = 0.0;
            phase_ = LockPhase::Splitting;
        }
        break;
    case LockPhase::Splitting:
        progress_ += clamped / kSplittingSeconds;
        if (progress_ >= 1.0) {
            progress_ = 0.0;
            phase_ = LockPhase::Typing;
        }
        break;
    case LockPhase::Closing:
        progress_ -= clamped / kClosingSeconds;
        if (progress_ <= 0.0) {
            progress_ = 0.0;
            phase_ = LockPhase::Hidden;
        }
        break;
    default:
        break;
    }
}

bool LockStateMachine::needs_frame() const noexcept {
    return phase_ == LockPhase::Opening || phase_ == LockPhase::Splitting ||
        phase_ == LockPhase::Closing;
}

} // namespace realmheart::ui::lockscreen
