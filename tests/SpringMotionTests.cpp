#include "animation/layered/SpringMotion.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

using realmheart::animation::layered::SpringMotion;

void spring_converges_without_instability() {
    SpringMotion spring(1.8, 0.82);
    spring.reset(0.0);
    spring.set_target(1.0);
    for (int frame = 0; frame < 240; ++frame) spring.advance(1.0 / 60.0);
    assert(std::abs(spring.value() - 1.0) < 0.001);
    assert(std::abs(spring.velocity()) < 0.01);
}

void variable_dt_tracks_fixed_step_solution() {
    SpringMotion fixed(1.4, 0.75);
    SpringMotion variable(1.4, 0.75);
    fixed.set_target(2.0);
    variable.set_target(2.0);
    for (int frame = 0; frame < 120; ++frame) fixed.advance(1.0 / 60.0);
    for (int frame = 0; frame < 120; ++frame) {
        variable.advance(frame % 2 == 0 ? 1.0 / 40.0 : 1.0 / 120.0);
    }
    assert(std::abs(fixed.value() - variable.value()) < 0.015);
    assert(std::abs(fixed.velocity() - variable.velocity()) < 0.03);
}

void invalid_or_negative_dt_is_ignored() {
    SpringMotion spring(2.0, 0.8);
    spring.set_target(1.0);
    spring.advance(-1.0);
    assert(spring.value() == 0.0);
    spring.advance(std::nan(""));
    assert(spring.value() == 0.0);
}

void long_pause_consumes_all_elapsed_time() {
    SpringMotion single_step(1.4, 0.75);
    SpringMotion substeps(1.4, 0.75);
    single_step.set_target(2.0);
    substeps.set_target(2.0);

    single_step.advance(1.0);
    for (int step = 0; step < 4; ++step) substeps.advance(0.25);

    assert(std::abs(single_step.value() - substeps.value()) < 0.0001);
    assert(std::abs(single_step.velocity() - substeps.velocity()) < 0.0001);
}

void extreme_finite_state_is_safely_bounded() {
    SpringMotion spring(2.0, 0.8);
    spring.reset(std::numeric_limits<double>::max(),
                 std::numeric_limits<double>::max());
    spring.set_target(-std::numeric_limits<double>::max());
    spring.advance(0.1);

    assert(std::isfinite(spring.value()));
    assert(std::isfinite(spring.velocity()));
    assert(std::isfinite(spring.target()));
}

} // namespace

int main() {
    spring_converges_without_instability();
    variable_dt_tracks_fixed_step_solution();
    invalid_or_negative_dt_is_ignored();
    long_pause_consumes_all_elapsed_time();
    extreme_finite_state_is_safely_bounded();
    std::cout << "Spring motion tests passed\n";
    return 0;
}
