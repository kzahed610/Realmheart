#include "effects/core/TransitionTimeline.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

using realmheart::effects::TransitionDurations;
using realmheart::effects::TransitionState;
using realmheart::effects::TransitionTimeline;

constexpr double kTolerance = 0.000001;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void require_near(double actual, double expected, const std::string& message) {
    require(std::abs(actual - expected) <= kTolerance, message);
}

void test_starts_hidden_and_idle() {
    TransitionTimeline timeline;
    require(timeline.state() == TransitionState::Hidden,
            "timeline must start hidden");
    require_near(timeline.progress(), 0.0,
                 "hidden timeline must start at zero progress");
    require(!timeline.active(), "hidden timeline must not require frame ticks");
    require(!timeline.target_visible(),
            "hidden timeline must target the hidden state");
}

void test_open_reaches_visible() {
    TransitionTimeline timeline({0.30, 0.18});
    timeline.open();
    require(timeline.state() == TransitionState::Opening,
            "open must begin the opening state");
    require(timeline.active(), "opening must require frame ticks");
    require(timeline.target_visible(), "opening must target visibility");

    require(timeline.advance(0.15),
            "half of the opening duration must remain active");
    require_near(timeline.progress(), 0.5,
                 "half of the opening duration must reach half progress");

    require(!timeline.advance(0.15),
            "the full opening duration must complete the transition");
    require(timeline.state() == TransitionState::Visible,
            "completed opening must finish visible");
    require_near(timeline.progress(), 1.0,
                 "visible timeline must finish at full progress");
    require(!timeline.active(), "visible timeline must stop frame ticks");
}

void test_close_reaches_hidden() {
    TransitionTimeline timeline({0.30, 0.18});
    timeline.snap_visible();
    timeline.close();

    require(timeline.state() == TransitionState::Closing,
            "close must begin the closing state");
    require(!timeline.target_visible(), "closing must target hidden");

    require(timeline.advance(0.09),
            "half of the closing duration must remain active");
    require_near(timeline.progress(), 0.5,
                 "half of the closing duration must reach half progress");

    require(!timeline.advance(0.09),
            "the full closing duration must complete the transition");
    require(timeline.state() == TransitionState::Hidden,
            "completed closing must finish hidden");
    require_near(timeline.progress(), 0.0,
                 "hidden timeline must finish at zero progress");
}

void test_reverses_opening_without_jump() {
    TransitionTimeline timeline({1.0, 0.5});
    timeline.open();
    require(timeline.advance(0.63),
            "partially opened transition must remain active");
    const double interrupted_progress = timeline.progress();

    timeline.close();
    require(timeline.state() == TransitionState::Closing,
            "closing during opening must reverse the state");
    require_near(timeline.progress(), interrupted_progress,
                 "reversal must preserve the current progress");

    require(timeline.advance(0.10),
            "partially reversed close must remain active");
    require_near(timeline.progress(), interrupted_progress - 0.20,
                 "reversed closing must advance from the interruption point");
}

void test_reverses_closing_without_jump() {
    TransitionTimeline timeline({0.5, 1.0});
    timeline.snap_visible();
    timeline.close();
    require(timeline.advance(0.70),
            "partially closed transition must remain active");
    const double interrupted_progress = timeline.progress();

    timeline.open();
    require(timeline.state() == TransitionState::Opening,
            "opening during close must reverse the state");
    require_near(timeline.progress(), interrupted_progress,
                 "reverse opening must preserve the current progress");

    require(timeline.advance(0.10),
            "partially reversed open must remain active");
    require_near(timeline.progress(), interrupted_progress + 0.20,
                 "reversed opening must advance from the interruption point");
}

void test_repeated_requests_are_idempotent() {
    TransitionTimeline timeline({1.0, 1.0});
    timeline.open();
    require(timeline.advance(0.25),
            "partial open must remain active");
    timeline.open();
    require_near(timeline.progress(), 0.25,
                 "repeated open must not restart progress");

    timeline.close();
    require(timeline.advance(0.10),
            "partial close must remain active");
    timeline.close();
    require_near(timeline.progress(), 0.15,
                 "repeated close must not restart progress");
}

void test_toggle_uses_current_destination() {
    TransitionTimeline timeline({1.0, 1.0});
    timeline.toggle();
    require(timeline.state() == TransitionState::Opening,
            "toggle from hidden must open");
    require(timeline.advance(0.4),
            "partial toggle-open must remain active");
    timeline.toggle();
    require(timeline.state() == TransitionState::Closing,
            "toggle while opening must reverse toward hidden");
    timeline.toggle();
    require(timeline.state() == TransitionState::Opening,
            "toggle while closing must reverse toward visible");
}

void test_invalid_elapsed_values_do_not_corrupt_progress() {
    TransitionTimeline timeline({1.0, 1.0});
    timeline.open();
    require(timeline.advance(0.25),
            "partial open must remain active before invalid deltas");
    const double progress = timeline.progress();

    require(timeline.advance(-1.0),
            "negative elapsed time must leave an active transition active");
    require_near(timeline.progress(), progress,
                 "negative elapsed time must not change progress");

    require(timeline.advance(std::numeric_limits<double>::quiet_NaN()),
            "NaN elapsed time must leave an active transition active");
    require_near(timeline.progress(), progress,
                 "NaN elapsed time must not change progress");
}

void test_large_elapsed_values_clamp_to_endpoints() {
    TransitionTimeline timeline({1.0, 1.0});
    timeline.open();
    require(!timeline.advance(100.0),
            "large opening delta must finish instead of overshooting");
    require_near(timeline.progress(), 1.0,
                 "large opening delta must clamp to one");

    timeline.close();
    require(!timeline.advance(100.0),
            "large closing delta must finish instead of undershooting");
    require_near(timeline.progress(), 0.0,
                 "large closing delta must clamp to zero");
}

void test_zero_or_invalid_durations_snap_safely() {
    TransitionTimeline zero({0.0, -1.0});
    zero.open();
    require(zero.state() == TransitionState::Visible,
            "zero open duration must snap visible");
    zero.close();
    require(zero.state() == TransitionState::Hidden,
            "invalid close duration must snap hidden");

    TransitionTimeline invalid({
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    });
    invalid.open();
    require(invalid.state() == TransitionState::Visible,
            "non-finite open duration must snap visible");
    invalid.close();
    require(invalid.state() == TransitionState::Hidden,
            "non-finite close duration must snap hidden");
}

} // namespace

int main() {
    test_starts_hidden_and_idle();
    test_open_reaches_visible();
    test_close_reaches_hidden();
    test_reverses_opening_without_jump();
    test_reverses_closing_without_jump();
    test_repeated_requests_are_idempotent();
    test_toggle_uses_current_destination();
    test_invalid_elapsed_values_do_not_corrupt_progress();
    test_large_elapsed_values_clamp_to_endpoints();
    test_zero_or_invalid_durations_snap_safely();
    std::cout << "Transition timeline tests passed\n";
    return 0;
}
