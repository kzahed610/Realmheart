#pragma once

#include <chrono>
#include <optional>

namespace realmheart::ui::powermenu {

class PowerMenuConfirmation {
public:
    enum class Action {
        Lock,
        Suspend,
        Logout,
        Reboot,
        PowerOff,
    };

    enum class Result {
        Armed,
        Confirmed,
    };

    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit PowerMenuConfirmation(
        std::chrono::milliseconds confirmation_delay = std::chrono::milliseconds{0},
        std::chrono::milliseconds confirmation_timeout = std::chrono::milliseconds{1200}
    );

    [[nodiscard]] Result activate(Action action, TimePoint now);
    void cancel();
    [[nodiscard]] std::optional<Action> armed_action() const;

private:
    std::optional<Action> armed_action_;
    TimePoint armed_at_{};
    std::chrono::milliseconds confirmation_delay_;
    std::chrono::milliseconds confirmation_timeout_;
};

} // namespace realmheart::ui::powermenu
