#pragma once

#include <cstdint>

namespace realmheart::wallpaper_native {

enum class NativeRestartDecision {
    Suppress,
    RestartAndReplay,
    FailClosed,
};

struct NativeRecoveryState {
    bool shutting_down = false;
    bool has_committed_wallpaper = false;
    std::uint32_t restart_attempts = 0;
};

[[nodiscard]] constexpr NativeRestartDecision native_restart_decision(
    NativeRecoveryState state
) noexcept {
    if (state.shutting_down) return NativeRestartDecision::Suppress;
    if (!state.has_committed_wallpaper || state.restart_attempts != 0) {
        return NativeRestartDecision::FailClosed;
    }
    return NativeRestartDecision::RestartAndReplay;
}

[[nodiscard]] constexpr bool native_command_ready(
    bool operation_succeeded,
    bool renderable,
    bool compositor_confirmed
) noexcept {
    return operation_succeeded && renderable && compositor_confirmed;
}

[[nodiscard]] constexpr bool native_output_should_recreate(
    bool output_still_available,
    bool layer_surface_closed
) noexcept {
    return output_still_available && layer_surface_closed;
}

[[nodiscard]] constexpr bool native_output_requires_redecode(
    int current_width,
    int current_height,
    int required_width,
    int required_height
) noexcept {
    return required_width > current_width || required_height > current_height;
}

} // namespace realmheart::wallpaper_native
