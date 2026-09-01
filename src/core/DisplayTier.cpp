#include "core/DisplayTier.hpp"

#include <algorithm>
#include <cmath>

namespace realmheart::core {
namespace {

constexpr int kP1080Height = 1080;
constexpr int kP1440Height = 1440;
constexpr int kP4KHeight = 2160;
constexpr int kP1080To1440Midpoint = (kP1080Height + kP1440Height) / 2;
constexpr int kP1440To4KMidpoint = (kP1440Height + kP4KHeight) / 2;

} // namespace

DisplayTier display_tier_for_logical_geometry(int width, int height) noexcept {
    if (width <= 0 || height <= 0) return DisplayTier::P1080;

    // Density/layout tiers follow the monitor's short edge rather than its raw
    // height. This preserves the same UI density when a 16:9 monitor is rotated
    // to portrait, while ultrawides/super-ultrawides with the same vertical
    // density remain in the same tier as their 16:9 counterparts.
    const int short_edge = std::min(width, height);
    // Snap to the nearest authored density family rather than treating every
    // value above 1080 as QHD. This keeps common 16:10 / fractionally-scaled
    // logical viewports (for example 1920x1200) on the visually-nearest 1080p
    // contract while still mapping 1440/1600-class displays to P1440.
    if (short_edge <= kP1080To1440Midpoint) return DisplayTier::P1080;
    if (short_edge <= kP1440To4KMidpoint) return DisplayTier::P1440;
    return DisplayTier::P4K;
}

DisplayTierSpec display_tier_spec(DisplayTier tier) noexcept {
    switch (tier) {
    case DisplayTier::P1080:
        return {
            .tier = DisplayTier::P1080,
            .directory = "1080p",
            .logical_width = 1920,
            .logical_height = 1080,
            .scale = 1.0,
        };
    case DisplayTier::P1440:
        return {
            .tier = DisplayTier::P1440,
            .directory = "1440p",
            .logical_width = 2560,
            .logical_height = 1440,
            .scale = 4.0 / 3.0,
        };
    case DisplayTier::P4K:
        return {
            .tier = DisplayTier::P4K,
            .directory = "4k",
            .logical_width = 3840,
            .logical_height = 2160,
            .scale = 2.0,
        };
    }

    return display_tier_spec(DisplayTier::P1080);
}

std::string_view display_tier_name(DisplayTier tier) noexcept {
    return display_tier_spec(tier).directory;
}

std::string_view display_tier_directory(DisplayTier tier) noexcept {
    return display_tier_spec(tier).directory;
}

int scale_dimension(int dimension, double scale) noexcept {
    if (dimension <= 0 || !std::isfinite(scale) || scale <= 0.0) {
        return 0;
    }

    const double scaled = static_cast<double>(dimension) * scale;
    if (!std::isfinite(scaled)) {
        return 0;
    }

    return static_cast<int>(std::lround(scaled));
}

} // namespace realmheart::core
