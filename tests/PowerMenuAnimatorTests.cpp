#include "ui/powermenu/PowerMenuAnimator.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

using namespace realmheart::ui::powermenu;

PowerMenuRig make_rig() {
    PowerMenuRig rig;
    rig.blink = {
        .half_asset = "eye-half",
        .closed_asset = "eye-closed",
        .minimum_interval_seconds = 0.35,
        .maximum_interval_seconds = 0.35,
        .double_blink_chance = 0.0,
        .duration_seconds = 0.70,
    };

    PowerMenuRigLayer dust{.asset = "dust", .z = 1, .animation = {}};
    dust.animation.type = PowerMenuAnimationType::Drift;
    dust.animation.translation = {2.0, -1.0};
    dust.animation.opacity_amplitude = 0.1;
    dust.animation.frequency = 0.2;
    dust.animation.phase = 0.0;
    rig.layers.push_back(dust);

    PowerMenuRigLayer spring{.asset = "strand", .z = 2, .animation = {}};
    spring.animation.type = PowerMenuAnimationType::Spring;
    spring.animation.translation = {1.0, 0.5};
    spring.animation.rotation_degrees = 1.5;
    spring.animation.frequency = 0.25;
    spring.animation.damping = 0.8;
    spring.animation.phase = 0.3;
    rig.layers.push_back(spring);

    PowerMenuRigLayer mesh{.asset = "hair", .z = 3, .animation = {}};
    mesh.animation.type = PowerMenuAnimationType::MeshFlow;
    mesh.animation.mesh.amplitude = 4.0;
    mesh.animation.flow.amplitude = 1.3;
    mesh.animation.flow.frequency = 0.18;
    mesh.animation.flow.phase = 0.2;
    rig.layers.push_back(mesh);

    PowerMenuRigLayer iris{.asset = "iris", .z = 4, .animation = {}};
    iris.animation.type = PowerMenuAnimationType::GlowMask;
    iris.animation.tint_role = "iris-gold";
    iris.animation.idle_minimum = 0.18;
    iris.animation.idle_maximum = 0.34;
    rig.layers.push_back(iris);

    PowerMenuRigLayer rune{.asset = "rune", .z = 5, .animation = {}};
    rune.animation.type = PowerMenuAnimationType::GlowMask;
    rune.animation.tint_role = "mana-gold";
    rune.animation.idle_minimum = 0.12;
    rune.animation.idle_maximum = 0.30;
    rig.layers.push_back(rune);
    return rig;
}

void lifecycle_opens_reverses_and_closes_safely() {
    PowerMenuAnimator animator(make_rig(), 42U);
    assert(animator.phase() == PowerMenuScenePhase::Hidden);
    assert(!animator.needs_frame());
    animator.open();
    assert(animator.phase() == PowerMenuScenePhase::Opening);
    animator.advance(0.20);
    const double partial_opacity = animator.frame().scene_opacity;
    assert(partial_opacity > 0.0 && partial_opacity < 1.0);
    // Lifecycle opacity belongs to the complete scene composition. Individual
    // layer opacity must remain intrinsic or the artwork fades twice and lags
    // behind sibling UI driven by scene_opacity.
    assert(std::abs(animator.frame().layers[1].opacity - 1.0) < 0.0001);
    animator.close();
    animator.advance(0.10);
    animator.open();
    animator.advance(1.0);
    assert(animator.phase() == PowerMenuScenePhase::Idle);
    assert(std::abs(animator.frame().scene_opacity - 1.0) < 0.0001);
    animator.set_confirming(true);
    assert(animator.phase() == PowerMenuScenePhase::Confirming);
    animator.close();
    assert(animator.phase() == PowerMenuScenePhase::Idle);
    animator.close();
    animator.advance(1.0);
    assert(animator.phase() == PowerMenuScenePhase::Hidden);
    assert(!animator.needs_frame());
}

void equal_seed_and_elapsed_produce_identical_frames() {
    PowerMenuAnimator left(make_rig(), 991U);
    PowerMenuAnimator right(make_rig(), 991U);
    left.open();
    right.open();
    left.advance(4.25);
    right.advance(4.25);
    const auto a = left.frame();
    const auto b = right.frame();
    assert(a.blink == b.blink);
    assert(a.layers.size() == b.layers.size());
    for (std::size_t index = 0; index < a.layers.size(); ++index) {
        assert(a.layers[index].translation_x == b.layers[index].translation_x);
        assert(a.layers[index].flow_displacement == b.layers[index].flow_displacement);
    }
}

void blink_glow_and_motion_remain_bounded() {
    PowerMenuAnimator animator(make_rig(), 7U);
    animator.open();
    animator.advance(1.0);
    bool saw_half = false;
    bool saw_closed = false;
    for (int frame = 0; frame < 240; ++frame) {
        animator.advance(1.0 / 120.0);
        const auto sample = animator.frame();
        saw_half = saw_half || sample.blink == PowerMenuBlinkState::Half;
        saw_closed = saw_closed || sample.blink == PowerMenuBlinkState::Closed;
        assert(sample.iris_glow >= 0.0 && sample.iris_glow <= 0.34);
        assert(sample.rune_glow >= 0.12 && sample.rune_glow <= 0.30);
        assert(std::abs(sample.layers[0].translation_x) <= 2.0);
        assert(std::abs(sample.layers[1].rotation_degrees) <= 1.5);
        assert(std::abs(sample.layers[2].macro_displacement) <= 4.0);
        assert(std::abs(sample.layers[2].flow_displacement) <= 1.3);
    }
    assert(saw_half);
    assert(saw_closed);
}

void blink_uses_authored_seven_tenths_second_timeline() {
    PowerMenuAnimator animator(make_rig(), 7U);
    animator.open();

    // This fixture's deterministic first deadline is 0.35 seconds. The
    // authored 700 ms blink is split half/closed/half as 200/300/200 ms.
    animator.advance(0.35);
    assert(animator.frame().blink == PowerMenuBlinkState::Half);
    animator.advance(0.19);
    assert(animator.frame().blink == PowerMenuBlinkState::Half);
    animator.advance(0.02);
    assert(animator.frame().blink == PowerMenuBlinkState::Closed);
    animator.advance(0.28);
    assert(animator.frame().blink == PowerMenuBlinkState::Closed);
    animator.advance(0.02);
    assert(animator.frame().blink == PowerMenuBlinkState::Half);
    animator.advance(0.18);
    assert(animator.frame().blink == PowerMenuBlinkState::Half);
    animator.advance(0.02);
    assert(animator.frame().blink == PowerMenuBlinkState::Open);
}

} // namespace

int main() {
    lifecycle_opens_reverses_and_closes_safely();
    equal_seed_and_elapsed_produce_identical_frames();
    blink_glow_and_motion_remain_bounded();
    blink_uses_authored_seven_tenths_second_timeline();
    std::cout << "Power menu animator tests passed\n";
    return 0;
}
