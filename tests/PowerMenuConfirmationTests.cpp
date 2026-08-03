#include "ui/powermenu/PowerMenuConfirmation.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {

using Confirmation = realmheart::ui::powermenu::PowerMenuConfirmation;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

void test_first_activation_only_arms_action() {
    Confirmation confirmation;
    const Confirmation::TimePoint now{};

    const auto result = confirmation.activate(Confirmation::Action::PowerOff, now);

    require(result == Confirmation::Result::Armed, "first activation must only arm");
    require(
        confirmation.armed_action() == Confirmation::Action::PowerOff,
        "first activation must remember the armed action"
    );
}

void test_quick_second_activation_confirms_action() {
    Confirmation confirmation(0ms, 1200ms);
    const Confirmation::TimePoint now{};
    static_cast<void>(confirmation.activate(Confirmation::Action::Reboot, now));

    const auto result = confirmation.activate(
        Confirmation::Action::Reboot,
        now + 120ms
    );

    require(result == Confirmation::Result::Confirmed, "deliberate second activation must confirm");
    require(!confirmation.armed_action().has_value(), "confirmed action must disarm immediately");
}

void test_cancel_clears_armed_action() {
    Confirmation confirmation;
    static_cast<void>(confirmation.activate(
        Confirmation::Action::Suspend,
        Confirmation::TimePoint{}
    ));

    confirmation.cancel();

    require(!confirmation.armed_action().has_value(), "cancel must clear the armed action");
}

void test_double_click_confirms() {
    Confirmation confirmation(0ms, 1200ms);
    const Confirmation::TimePoint now{};
    static_cast<void>(confirmation.activate(Confirmation::Action::Lock, now));

    const auto result = confirmation.activate(Confirmation::Action::Lock, now + 100ms);

    require(result == Confirmation::Result::Confirmed, "rapid second click must confirm");
}

void test_expired_repeat_rearms_instead_of_confirming() {
    Confirmation confirmation(0ms, 1200ms);
    const Confirmation::TimePoint now{};
    static_cast<void>(confirmation.activate(Confirmation::Action::Logout, now));

    const auto result = confirmation.activate(Confirmation::Action::Logout, now + 1300ms);

    require(result == Confirmation::Result::Armed, "expired repeat must rearm");
}

void test_different_action_rearms_instead_of_confirming() {
    Confirmation confirmation(0ms, 1200ms);
    const Confirmation::TimePoint now{};
    static_cast<void>(confirmation.activate(Confirmation::Action::Suspend, now));

    const auto result = confirmation.activate(Confirmation::Action::PowerOff, now + 1s);

    require(result == Confirmation::Result::Armed, "different action must rearm");
    require(
        confirmation.armed_action() == Confirmation::Action::PowerOff,
        "different action must replace the armed action"
    );
}

} // namespace

int main() {
    test_first_activation_only_arms_action();
    test_quick_second_activation_confirms_action();
    test_cancel_clears_armed_action();
    test_double_click_confirms();
    test_expired_repeat_rearms_instead_of_confirming();
    test_different_action_rearms_instead_of_confirming();
    std::cout << "PowerMenuConfirmation tests passed\n";
    return 0;
}
