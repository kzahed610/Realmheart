#include "ui/workspace/animation/WorkspaceMorphShaderData.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::ui::workspace::animation {
namespace {

[[nodiscard]] float normalized(double value, double extent) noexcept {
    if (!std::isfinite(value) || !std::isfinite(extent) || extent <= 0.0) {
        return 0.0F;
    }
    return static_cast<float>(std::clamp(value / extent, 0.0, 1.0));
}

} // namespace

WorkspaceMorphShaderGeometry build_workspace_morph_shader_geometry(
    const WorkspaceMorphLayout& layout
) noexcept {
    WorkspaceMorphShaderGeometry geometry;
    const double width = std::max(layout.width, 1.0);
    const double height = std::max(layout.height, 1.0);

    double origin_x = 0.0;
    double origin_y = 0.0;
    for (std::size_t index = 0; index < layout.bands.size(); ++index) {
        const auto& band = layout.bands[index];
        geometry.band_top[index] = normalized(band.destination.y, height);
        geometry.band_bottom[index] = normalized(
            band.destination.y + band.destination.height,
            height
        );
        geometry.element_style[index] = static_cast<float>(band.style_index);
        geometry.source_y[index] = normalized(band.source.center_y(), height);
        origin_x += band.source.center_x();
        origin_y += band.source.center_y();
    }

    constexpr double count = static_cast<double>(kWorkspaceMorphBandCount);
    geometry.origin[0] = normalized(origin_x / count, width);
    geometry.origin[1] = normalized(origin_y / count, height);
    return geometry;
}

WorkspaceMorphShaderFrame build_workspace_morph_shader_frame(
    const WorkspaceMorphLayout& layout,
    const WorkspaceMorphFrame& frame
) noexcept {
    WorkspaceMorphShaderFrame shader_frame;
    const double width = std::max(layout.width, 1.0);
    const double height = std::max(layout.height, 1.0);
    for (std::size_t index = 0; index < frame.bands.size(); ++index) {
        const auto& reveal = frame.bands[index].reveal_clip;
        shader_frame.reveal_left_x[index] = normalized(reveal.x, width);
        shader_frame.front_x[index] = normalized(
            reveal.x + reveal.width,
            width
        );
        shader_frame.front_top[index] = normalized(reveal.y, height);
        shader_frame.front_bottom[index] = normalized(
            reveal.y + reveal.height,
            height
        );
    }
    return shader_frame;
}

} // namespace realmheart::ui::workspace::animation
