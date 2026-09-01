#include "core/MonitorContext.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::core {
namespace {

constexpr double kUltrawideRatio = 2.0;
constexpr double kSuperUltrawideRatio = 3.0;

} // namespace

int MonitorContext::physical_width_hint() const noexcept {
    return scale_dimension(logical_width, scale);
}

int MonitorContext::physical_height_hint() const noexcept {
    return scale_dimension(logical_height, scale);
}

MonitorAspectClass monitor_aspect_class_for_logical_geometry(
    int width,
    int height
) noexcept {
    if (width <= 0 || height <= 0) return MonitorAspectClass::Standard;
    if (width < height) return MonitorAspectClass::Portrait;

    const double ratio = static_cast<double>(width) / static_cast<double>(height);
    if (ratio >= kSuperUltrawideRatio) return MonitorAspectClass::SuperUltrawide;
    if (ratio >= kUltrawideRatio) return MonitorAspectClass::Ultrawide;
    return MonitorAspectClass::Standard;
}

std::string_view monitor_aspect_class_name(MonitorAspectClass aspect) noexcept {
    switch (aspect) {
    case MonitorAspectClass::Portrait: return "portrait";
    case MonitorAspectClass::Standard: return "standard";
    case MonitorAspectClass::Ultrawide: return "ultrawide";
    case MonitorAspectClass::SuperUltrawide: return "super-ultrawide";
    }
    return "standard";
}

MonitorContext monitor_context_for_geometry(
    int index,
    int x,
    int y,
    int logical_width,
    int logical_height,
    double scale
) noexcept {
    const double normalized_scale = std::isfinite(scale) && scale > 0.0
        ? scale
        : 1.0;
    const int safe_width = logical_width > 0 ? logical_width : 1920;
    const int safe_height = logical_height > 0 ? logical_height : 1080;

    MonitorContext context;
    context.index = std::max(index, 0);
    context.x = x;
    context.y = y;
    context.logical_width = safe_width;
    context.logical_height = safe_height;
    context.scale = normalized_scale;
    context.layout_tier = display_tier_for_logical_geometry(safe_width, safe_height);
    context.asset_tier = display_tier_for_logical_geometry(
        scale_dimension(safe_width, normalized_scale),
        scale_dimension(safe_height, normalized_scale)
    );
    context.aspect = monitor_aspect_class_for_logical_geometry(
        safe_width,
        safe_height
    );
    return context;
}

} // namespace realmheart::core
