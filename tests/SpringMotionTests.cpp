#include "animation/layered/SpringMotion.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

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

} // namespace

int main() {
    spring_converges_without_instability();
    variable_dt_tracks_fixed_step_solution();
    invalid_or_negative_dt_is_ignored();
    std::cout << "Spring motion tests passed\n";
    return 0;
}
