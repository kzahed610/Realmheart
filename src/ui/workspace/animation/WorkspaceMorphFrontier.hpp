#pragma once

#include "ui/workspace/animation/WorkspaceMorphModel.hpp"

#include <array>
#include <cstddef>

namespace realmheart::ui::workspace::animation {

inline constexpr std::size_t kWorkspaceMorphFrontierSampleCount = 41;
inline constexpr std::size_t kWorkspaceMorphFrontierParticleCount = 14;

struct WorkspaceMorphFrontierPoint {
    double x = 0.0;
    double y = 0.0;
};

struct WorkspaceMorphFrontierParticle {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double opacity = 0.0;
};

struct WorkspaceMorphFrontier {
    std::array<WorkspaceMorphFrontierPoint,
               kWorkspaceMorphFrontierSampleCount> points{};
    std::array<WorkspaceMorphFrontierParticle,
               kWorkspaceMorphFrontierParticleCount> particles{};
    double reveal_left_x = 0.0;
    double glow_opacity = 0.0;
    double core_opacity = 0.0;
    double glow_half_width = 0.0;
    double core_half_width = 0.0;
};

[[nodiscard]] WorkspaceMorphFrontier build_workspace_morph_frontier(
    std::size_t style_index,
    const WorkspaceMorphRect& reveal_clip,
    double source_right_x,
    double layout_width,
    double progress
) noexcept;

[[nodiscard]] double workspace_morph_frontier_x_at(
    const WorkspaceMorphFrontier& frontier,
    double y
) noexcept;

} // namespace realmheart::ui::workspace::animation
