#include "ui/workspace/animation/WorkspaceMorphFrontier.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

using realmheart::ui::workspace::animation::WorkspaceMorphFrontier;
using realmheart::ui::workspace::animation::WorkspaceMorphRect;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) > 0.0001) {
        throw std::runtime_error(message);
    }
}

WorkspaceMorphFrontier build(std::size_t style, double progress) {
    return realmheart::ui::workspace::animation::
        build_workspace_morph_frontier(
            style,
            WorkspaceMorphRect{24.0, 120.0, 736.0, 280.0},
            49.0,
            1920.0,
            progress
        );
}

void test_endpoints_have_no_transition_effect() {
    const auto hidden = build(0U, 0.0);
    const auto visible = build(0U, 1.0);
    require_near(hidden.glow_opacity, 0.0,
                 "hidden frontier must not retain glow");
    require_near(hidden.core_opacity, 0.0,
                 "hidden frontier must not retain a core");
    require_near(visible.glow_opacity, 0.0,
                 "visible handoff must not retain glow");
    require_near(visible.core_opacity, 0.0,
                 "visible handoff must not retain a core");
    for (const auto& particle : visible.particles) {
        require_near(particle.opacity, 0.0,
                     "visible handoff must release particles");
    }
}

void test_mid_transition_frontier_is_irregular_and_bounded() {
    const auto frontier = build(0U, 0.52);
    require_near(frontier.reveal_left_x, 24.0,
                 "frontier reveal path must preserve its local left root");
    double minimum = frontier.points.front().x;
    double maximum = minimum;
    for (const auto& point : frontier.points) {
        minimum = std::min(minimum, point.x);
        maximum = std::max(maximum, point.x);
        require(point.x >= 0.0 && point.x <= 1920.0,
                "frontier samples must remain inside the stage");
        require(point.y >= 120.0 && point.y <= 400.0,
                "frontier samples must remain inside the band");
    }
    require(maximum - minimum > 18.0,
            "fire frontier must visibly break up the rectangular edge");
    require(frontier.glow_opacity > 0.6,
            "mid-transition frontier must have a visible glow");
    require(frontier.core_opacity > 0.8,
            "mid-transition frontier must have a bright core");
}

void test_elemental_styles_are_not_the_same_curve() {
    const auto fire = build(0U, 0.47);
    const auto water = build(1U, 0.47);
    const auto wind = build(2U, 0.47);
    const auto earth = build(3U, 0.47);

    double fire_water_difference = 0.0;
    double water_wind_difference = 0.0;
    double wind_earth_difference = 0.0;
    for (std::size_t index = 0; index < fire.points.size(); ++index) {
        fire_water_difference += std::abs(
            fire.points[index].x - water.points[index].x
        );
        water_wind_difference += std::abs(
            water.points[index].x - wind.points[index].x
        );
        wind_earth_difference += std::abs(
            wind.points[index].x - earth.points[index].x
        );
    }
    require(fire_water_difference > 120.0,
            "fire and water must not share one generic frontier");
    require(water_wind_difference > 120.0,
            "water and wind must not share one generic frontier");
    require(wind_earth_difference > 120.0,
            "wind and earth must not share one generic frontier");
}

void test_particles_follow_the_frontier() {
    for (std::size_t style = 0; style < 4U; ++style) {
        const auto frontier = build(style, 0.55);
        std::size_t visible_particles = 0U;
        for (const auto& particle : frontier.particles) {
            require(particle.x >= 0.0 && particle.x <= 1920.0,
                    "particles must remain inside the stage");
            require(particle.y >= 120.0 && particle.y <= 400.0,
                    "particles must remain inside the band");
            require(particle.width > 0.0 && particle.height > 0.0,
                    "particles must have positive geometry");
            if (particle.opacity > 0.1) ++visible_particles;
        }
        require(visible_particles >= 8U,
                "each elemental frontier must produce restrained particles");
    }
}


void test_absorption_fades_in_place_at_the_rune() {
    const auto faint = build(0U, 0.08);
    const auto brighter = build(0U, 0.12);

    require(faint.glow_opacity > 0.0 && brighter.glow_opacity > 0.0,
            "absorption glow must remain visible while fading");
    require(brighter.glow_opacity > faint.glow_opacity,
            "opening must brighten the same stationary absorption glow");
    require_near(faint.glow_half_width, brighter.glow_half_width,
                 "absorption glow width must not translate by collapsing");
    require_near(faint.core_half_width, brighter.core_half_width,
                 "absorption core width must remain fixed while fading");

    for (const auto& point : faint.points) {
        require_near(point.x, 49.0,
                     "faint absorption frontier must stay at the rune edge");
    }
    for (const auto& point : brighter.points) {
        require_near(point.x, 49.0,
                     "bright absorption frontier must stay at the rune edge");
    }
    for (const auto& particle : faint.particles) {
        require_near(particle.opacity, 0.0,
                     "particles must not drift away during final absorption");
    }
    for (const auto& particle : brighter.particles) {
        require_near(particle.opacity, 0.0,
                     "particles must remain suppressed until travel begins");
    }
}

void test_frontier_interpolation_matches_samples() {
    const auto frontier = build(2U, 0.61);
    const auto& point = frontier.points[17U];
    require_near(
        realmheart::ui::workspace::animation::workspace_morph_frontier_x_at(
            frontier,
            point.y
        ),
        point.x,
        "frontier lookup must reproduce exact sample positions"
    );
}

} // namespace

int main() {
    try {
        test_endpoints_have_no_transition_effect();
        test_mid_transition_frontier_is_irregular_and_bounded();
        test_elemental_styles_are_not_the_same_curve();
        test_particles_follow_the_frontier();
        test_absorption_fades_in_place_at_the_rune();
        test_frontier_interpolation_matches_samples();
    } catch (const std::exception& error) {
        std::cerr << "WorkspaceMorphFrontierTests failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "Workspace morph frontier tests passed\n";
    return 0;
}
