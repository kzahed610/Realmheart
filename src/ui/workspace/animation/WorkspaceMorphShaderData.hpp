#pragma once

#include "ui/workspace/animation/WorkspaceMorphModel.hpp"

#include <array>

namespace realmheart::ui::workspace::animation {

struct WorkspaceMorphShaderGeometry {
    std::array<float, 2> origin{{0.0F, 0.5F}};
    std::array<float, kWorkspaceMorphBandCount> band_top{};
    std::array<float, kWorkspaceMorphBandCount> band_bottom{};
    std::array<float, kWorkspaceMorphBandCount> element_style{};
    std::array<float, kWorkspaceMorphBandCount> source_y{};
};

struct WorkspaceMorphShaderFrame {
    std::array<float, kWorkspaceMorphBandCount> reveal_left_x{};
    std::array<float, kWorkspaceMorphBandCount> front_x{};
    std::array<float, kWorkspaceMorphBandCount> front_top{};
    std::array<float, kWorkspaceMorphBandCount> front_bottom{};
};

[[nodiscard]] WorkspaceMorphShaderGeometry
build_workspace_morph_shader_geometry(
    const WorkspaceMorphLayout& layout
) noexcept;

[[nodiscard]] WorkspaceMorphShaderFrame build_workspace_morph_shader_frame(
    const WorkspaceMorphLayout& layout,
    const WorkspaceMorphFrame& frame
) noexcept;

} // namespace realmheart::ui::workspace::animation
