#include "ui/workspace/animation/WorkspaceMorphFrontier.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace realmheart::ui::workspace::animation {
namespace {

constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] double clamp_unit(double value) noexcept {
    if (!std::isfinite(value)) return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] double smoothstep01(double value) noexcept {
    const double t = clamp_unit(value);
    return t * t * (3.0 - 2.0 * t);
}

[[nodiscard]] double hash01(std::uint32_t value) noexcept {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return static_cast<double>(value & 0x00ffffffU) /
        static_cast<double>(0x01000000U);
}

[[nodiscard]] double signed_hash(std::uint32_t value) noexcept {
    return hash01(value) * 2.0 - 1.0;
}

[[nodiscard]] double frontier_offset(
    std::size_t style_index,
    double vertical,
    double progress
) noexcept {
    const double phase = progress * 2.0 * kPi;
    switch (style_index % 4U) {
    case 0U: { // Fire: sharp licking tongues with a turbulent core.
        const double tongue = std::max(
            0.0,
            std::sin(vertical * 9.0 * kPi - phase * 1.7)
        );
        return 10.0 * std::sin(vertical * 7.0 * kPi + phase * 1.3) +
            6.0 * std::sin(vertical * 23.0 * kPi - phase * 2.1) +
            13.0 * tongue * tongue * tongue;
    }
    case 1U: // Water: broad wave with smaller refraction ripples.
        return 17.0 * std::sin(vertical * 3.2 * kPi - phase * 0.72) +
            5.0 * std::sin(vertical * 10.0 * kPi + phase * 0.48);
    case 2U: // Wind: long pressure curves with light high-frequency breakup.
        return 13.0 * std::sin(vertical * 5.0 * kPi + phase * 1.05) +
            9.0 * std::sin(vertical * 1.8 * kPi - phase * 0.82) +
            3.5 * std::sin(vertical * 17.0 * kPi + phase * 1.8);
    default: { // Earth: heavier, fractured stepping rather than a smooth wave.
        const auto vertical_cell = static_cast<std::uint32_t>(vertical * 19.0);
        const double time = progress * 8.0;
        const auto time_cell = static_cast<std::uint32_t>(std::floor(time));
        const double time_mix = smoothstep01(time - std::floor(time));
        const double fracture_from = signed_hash(
            vertical_cell + time_cell * 31U + 97U
        );
        const double fracture_to = signed_hash(
            vertical_cell + (time_cell + 1U) * 31U + 97U
        );
        const double fracture = fracture_from +
            (fracture_to - fracture_from) * time_mix;
        return 8.0 * std::sin(vertical * 4.0 * kPi - phase * 0.45) +
            10.0 * fracture;
    }
    }
}

[[nodiscard]] WorkspaceMorphFrontierParticle particle_for(
    std::size_t style_index,
    std::size_t particle_index,
    const WorkspaceMorphFrontier& frontier,
    double top,
    double height,
    double layout_width,
    double progress,
    double activity
) noexcept {
    const std::uint32_t seed = static_cast<std::uint32_t>(
        0x9e3779b9U * static_cast<std::uint32_t>(particle_index + 1U) +
        0x85ebca6bU * static_cast<std::uint32_t>(style_index + 1U)
    );
    const double phase = std::fmod(
        hash01(seed ^ 0x68bc21ebU) + progress *
            (0.34 + hash01(seed ^ 0x02e5be93U) * 0.54),
        1.0
    );
    const double y_fraction = std::fmod(
        hash01(seed ^ 0xa511e9b3U) + phase *
            (0.18 + hash01(seed ^ 0x63d83595U) * 0.24),
        1.0
    );
    const double y = top + height * y_fraction;
    const double front_x = workspace_morph_frontier_x_at(frontier, y);
    const double random_a = hash01(seed ^ 0xc2b2ae35U);
    const double random_b = hash01(seed ^ 0x27d4eb2fU);
    const double random_c = hash01(seed ^ 0x165667b1U);

    WorkspaceMorphFrontierParticle particle;
    particle.y = y;
    switch (style_index % 4U) {
    case 0U: // embers
        particle.x = front_x + 6.0 + phase * 48.0 + random_a * 18.0;
        particle.width = 1.8 + random_b * 3.2;
        particle.height = 2.5 + random_c * 7.0;
        particle.opacity = activity * (0.32 + random_a * 0.54) *
            (1.0 - phase * 0.55);
        break;
    case 1U: // droplets / caustic beads
        particle.x = front_x - 8.0 + phase * 34.0 + random_a * 14.0;
        particle.width = 2.0 + random_b * 4.0;
        particle.height = 2.0 + random_c * 4.0;
        particle.opacity = activity * (0.24 + random_a * 0.42);
        break;
    case 2U: // directional wisps
        particle.x = front_x + 10.0 + phase * 54.0 + random_a * 22.0;
        particle.width = 12.0 + random_b * 28.0;
        particle.height = 1.0 + random_c * 2.0;
        particle.opacity = activity * (0.18 + random_a * 0.38) *
            (1.0 - phase * 0.40);
        break;
    default: // dust and small fragments
        particle.x = front_x + 2.0 + phase * 38.0 + random_a * 18.0;
        particle.width = 2.5 + random_b * 6.0;
        particle.height = 2.5 + random_c * 7.0;
        particle.opacity = activity * (0.20 + random_a * 0.46) *
            (1.0 - phase * 0.35);
        break;
    }
    particle.x = std::clamp(particle.x, 0.0, std::max(0.0, layout_width));
    return particle;
}

} // namespace

WorkspaceMorphFrontier build_workspace_morph_frontier(
    std::size_t style_index,
    const WorkspaceMorphRect& reveal_clip,
    double source_right_x,
    double layout_width,
    double progress
) noexcept {
    WorkspaceMorphFrontier frontier;
    const double safe_width = std::max(
        1.0,
        std::isfinite(layout_width) ? layout_width : 1.0
    );
    const double top = std::isfinite(reveal_clip.y) ? reveal_clip.y : 0.0;
    const double height = std::max(
        0.0,
        std::isfinite(reveal_clip.height) ? reveal_clip.height : 0.0
    );
    const double left_x = std::clamp(
        std::isfinite(reveal_clip.x) ? reveal_clip.x : 0.0,
        0.0,
        safe_width
    );
    const double reveal_right = std::clamp(
        std::isfinite(reveal_clip.x + reveal_clip.width)
            ? reveal_clip.x + reveal_clip.width
            : left_x,
        left_x,
        safe_width
    );
    const double source_right = std::clamp(
        std::isfinite(source_right_x) ? source_right_x : reveal_right,
        left_x,
        safe_width
    );
    const double p = clamp_unit(progress);
    const double enter = smoothstep01((p - 0.025) / 0.14);
    const double settle = smoothstep01((p - 0.88) / 0.12);
    const double activity = enter * (1.0 - settle);

    // The last absorption phase is alpha-only. Once the travelling frontier
    // reaches the captured rune edge, hold it there while its glow fades.
    // This removes the tiny rightward-looking drift caused by a still-changing
    // frontier width/breakup during the final frames. Opening samples the same
    // function forward: the glow ignites at the rune, then begins travelling.
    const double travel = smoothstep01((p - 0.160) / 0.110);
    const double target_right = std::max(reveal_right, source_right);
    const double base_x = source_right +
        (target_right - source_right) * travel;
    const double amplitude = activity * travel * (0.88 + 0.12 *
        std::sin(p * 2.0 * kPi));
    frontier.reveal_left_x = left_x;

    for (std::size_t index = 0; index < frontier.points.size(); ++index) {
        const double vertical = frontier.points.size() > 1U
            ? static_cast<double>(index) /
                static_cast<double>(frontier.points.size() - 1U)
            : 0.0;
        const double y = top + height * vertical;
        const double offset = frontier_offset(style_index, vertical, p) *
            amplitude;
        frontier.points[index] = {
            std::clamp(base_x + offset, left_x, safe_width),
            y,
        };
    }

    frontier.glow_opacity = activity * 0.72;
    frontier.core_opacity = activity * 0.96;
    // Keep the absorption silhouette spatially fixed and let opacity do the
    // disappearing. Width changes only once the frontier actually travels.
    frontier.glow_half_width = 18.0 + travel * 5.0;
    frontier.core_half_width = 2.8 + travel * 0.6;

    const double particle_activity = activity * travel;
    for (std::size_t index = 0; index < frontier.particles.size(); ++index) {
        frontier.particles[index] = particle_for(
            style_index,
            index,
            frontier,
            top,
            height,
            safe_width,
            p,
            particle_activity
        );
    }
    return frontier;
}

double workspace_morph_frontier_x_at(
    const WorkspaceMorphFrontier& frontier,
    double y
) noexcept {
    if (!std::isfinite(y)) return frontier.points.front().x;
    if (y <= frontier.points.front().y) return frontier.points.front().x;
    if (y >= frontier.points.back().y) return frontier.points.back().x;

    for (std::size_t index = 1; index < frontier.points.size(); ++index) {
        const auto& from = frontier.points[index - 1U];
        const auto& to = frontier.points[index];
        if (y > to.y) continue;
        const double span = std::max(to.y - from.y, 0.000001);
        const double t = std::clamp((y - from.y) / span, 0.0, 1.0);
        return from.x + (to.x - from.x) * t;
    }
    return frontier.points.back().x;
}

} // namespace realmheart::ui::workspace::animation
