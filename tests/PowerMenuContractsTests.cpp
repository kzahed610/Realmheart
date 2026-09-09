#include "ui/powermenu/PowerMenuContracts.hpp"

#include <cassert>
#include <iostream>

using namespace realmheart::ui::powermenu;

void test_media_error_is_terminal() {
    assert(media_acquisition_state(false, false) == MediaAcquisitionState::Pending);
    assert(media_acquisition_state(true, false) == MediaAcquisitionState::Ready);
    assert(media_acquisition_state(true, true) == MediaAcquisitionState::Failed);
}

void test_action_hides_only_after_completion() {
    assert(action_completion(false, false) == ActionCompletion::Failed);
    assert(action_completion(true, false) == ActionCompletion::Pending);
    assert(action_completion(true, true) == ActionCompletion::Completed);
    assert(!action_completion_allows_hide(ActionCompletion::Failed));
    assert(!action_completion_allows_hide(ActionCompletion::Pending));
    assert(action_completion_allows_hide(ActionCompletion::Completed));
}

void test_failed_ripple_has_no_live_handoff_work() {
    const RippleFailureFallback fallback = terminal_ripple_failure();
    assert(fallback.handoff_pending == false);
    assert(fallback.handoff_active == false);
    assert(fallback.timer_needed == false);
    assert(fallback.opacity_fallback == true);
}

void test_monitor_binding_uses_zero_fallback() {
    assert(effective_monitor_index(3, true) == 3);
    assert(effective_monitor_index(3, false) == 0);
    assert(effective_monitor_index(-1, false) == -1);
}

void test_close_grace_covers_animation() {
    assert(kPowerMenuCloseGraceMs >= 1050);
}

int main() {
    test_media_error_is_terminal();
    test_action_hides_only_after_completion();
    test_failed_ripple_has_no_live_handoff_work();
    test_monitor_binding_uses_zero_fallback();
    test_close_grace_covers_animation();
    std::cout << "All Power Menu contract tests PASSED\n";
    return 0;
}
