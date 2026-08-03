#include "ui/powermenu/PowerMenuConfirmation.hpp"

namespace realmheart::ui::powermenu {

PowerMenuConfirmation::PowerMenuConfirmation(
    std::chrono::milliseconds confirmation_delay,
    std::chrono::milliseconds confirmation_timeout
) : confirmation_delay_(confirmation_delay),
    confirmation_timeout_(confirmation_timeout) {}

PowerMenuConfirmation::Result PowerMenuConfirmation::activate(
    Action action,
    TimePoint now
) {
    if (armed_action_ == action) {
        const auto elapsed = now - armed_at_;
        if (elapsed >= confirmation_delay_ && elapsed <= confirmation_timeout_) {
            armed_action_.reset();
            return Result::Confirmed;
        }
    }

    armed_action_ = action;
    armed_at_ = now;
    return Result::Armed;
}

void PowerMenuConfirmation::cancel() {
    armed_action_.reset();
}

std::optional<PowerMenuConfirmation::Action>
PowerMenuConfirmation::armed_action() const {
    return armed_action_;
}

} // namespace realmheart::ui::powermenu
