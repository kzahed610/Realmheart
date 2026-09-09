#pragma once

namespace realmheart::ui::powermenu {

enum class MediaAcquisitionState {
    Pending,
    Ready,
    Failed,
};

constexpr MediaAcquisitionState media_acquisition_state(
    bool prepared,
    bool has_error
) noexcept {
    if (has_error) return MediaAcquisitionState::Failed;
    return prepared ? MediaAcquisitionState::Ready : MediaAcquisitionState::Pending;
}

enum class ActionCompletion {
    Failed,
    Pending,
    Completed,
};

constexpr bool action_completion_allows_hide(ActionCompletion completion) noexcept {
    return completion == ActionCompletion::Completed;
}

constexpr ActionCompletion action_completion(
    bool launch_succeeded,
    bool completion_observed
) noexcept {
    if (!launch_succeeded) return ActionCompletion::Failed;
    return completion_observed
        ? ActionCompletion::Completed
        : ActionCompletion::Pending;
}

struct RippleFailureFallback {
    bool handoff_pending = false;
    bool handoff_active = false;
    bool timer_needed = false;
    bool opacity_fallback = true;
};

constexpr RippleFailureFallback terminal_ripple_failure() noexcept {
    return {};
}

constexpr int effective_monitor_index(int requested, bool monitor_bound) noexcept {
    if (requested < 0 || monitor_bound) return requested;
    return 0;
}

// PowerMenuVideoState's closing transition is 1.05 seconds. Keep a small
// compositor/process scheduling margin before signalling the helper process.
inline constexpr unsigned int kPowerMenuCloseGraceMs = 1200;

} // namespace realmheart::ui::powermenu
