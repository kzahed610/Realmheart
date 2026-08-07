#include "ui/workspace/animation/WorkspaceMorphRendererState.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

using realmheart::ui::workspace::animation::WorkspaceMorphRendererState;
using realmheart::ui::workspace::animation::workspace_morph_overlay_opacity;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) > 0.000001) {
        throw std::runtime_error(message);
    }
}

void test_opening_lifecycle_and_readiness() {
    WorkspaceMorphRendererState state;
    state.begin(true);

    require(state.active(), "begin must activate the renderer state");
    require(state.opening(), "opening begin must preserve its direction");
    require_near(state.progress(), 0.0,
                 "opening begin must start at the hidden endpoint");
    require(!state.frame_ready(),
            "begin must clear readiness from a previous transition");

    state.update(0.42, true);
    state.mark_frame_ready();
    require_near(state.progress(), 0.42,
                 "update must store the authoritative timeline progress");
    require(state.frame_ready(),
            "the first successful draw must open the readiness gate");
}

void test_reversal_keeps_current_progress() {
    WorkspaceMorphRendererState state;
    state.begin(true);
    state.update(0.63, true);
    state.mark_frame_ready();
    state.update(0.63, false);

    require(state.active(), "reversal must keep the same active session");
    require(!state.opening(), "reversal must update direction in place");
    require_near(state.progress(), 0.63,
                 "reversal must not restart progress at an endpoint");
    require(state.frame_ready(),
            "reversal must reuse the already prepared shader session");
}

void test_finish_and_repeated_begin_reset_transient_state() {
    WorkspaceMorphRendererState state;
    state.begin(false);
    require_near(state.progress(), 1.0,
                 "closing begin must start at the visible endpoint");
    state.mark_frame_ready();
    state.finish();

    require(!state.active(), "finish must deactivate the renderer state");
    require(!state.frame_ready(), "finish must clear frame readiness");

    state.begin(true);
    require(state.active(), "a finished state must support a new session");
    require(!state.frame_ready(),
            "a new session must not inherit readiness from the old one");
    require_near(state.progress(), 0.0,
                 "a new opening session must reset to the hidden endpoint");
}

void test_updates_are_clamped_and_idle_updates_are_ignored() {
    WorkspaceMorphRendererState state;
    state.update(0.5, false);
    require_near(state.progress(), 0.0,
                 "an idle update must not mutate renderer state");

    state.begin(true);
    state.update(4.0, true);
    require_near(state.progress(), 1.0,
                 "progress above one must clamp to one");
    state.update(-2.0, false);
    require_near(state.progress(), 0.0,
                 "progress below zero must clamp to zero");
}

void test_overlay_opacity_is_safe_at_readiness_and_handoff_frames() {
    const double priming = workspace_morph_overlay_opacity(0.5, false);
    require_near(priming, 1.0,
                 "an unready GL host must remain in the GTK snapshot tree");
    require_near(workspace_morph_overlay_opacity(0.0, true), 0.0,
                 "the hidden endpoint must not overlay the native geometry");
    require_near(workspace_morph_overlay_opacity(1.0, true), 0.0,
                 "the visible endpoint must not double-composite the scene");
    require(workspace_morph_overlay_opacity(0.5, true) > 0.999,
            "the elemental layer must reach full strength mid-transition");
    require(workspace_morph_overlay_opacity(0.01, true) > 0.0 &&
                workspace_morph_overlay_opacity(0.01, true) < 1.0,
            "the hidden handoff must fade smoothly instead of snapping");
}

} // namespace

int main() {
    try {
        test_opening_lifecycle_and_readiness();
        test_reversal_keeps_current_progress();
        test_finish_and_repeated_begin_reset_transient_state();
        test_updates_are_clamped_and_idle_updates_are_ignored();
        test_overlay_opacity_is_safe_at_readiness_and_handoff_frames();
    } catch (const std::exception& error) {
        std::cerr << "WorkspaceMorphRendererStateTests failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "Workspace morph renderer state tests passed\n";
    return 0;
}
