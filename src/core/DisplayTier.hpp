#pragma once

#include <string_view>

namespace realmheart::core {

enum class DisplayTier {
    P1080,
    P1440,
    P4K,
};

struct DisplayTierSpec {
    DisplayTier tier;
    std::string_view directory;
    int logical_width;
    int logical_height;
    double scale;
};

[[nodiscard]] DisplayTier display_tier_for_logical_geometry(
    int width,
    int height
) noexcept;

[[nodiscard]] DisplayTierSpec display_tier_spec(DisplayTier tier) noexcept;

[[nodiscard]] std::string_view display_tier_name(DisplayTier tier) noexcept;
[[nodiscard]] std::string_view display_tier_directory(DisplayTier tier) noexcept;

// Scales a non-negative raster dimension using round-half-away-from-zero.
[[nodiscard]] int scale_dimension(int dimension, double scale) noexcept;

} // namespace realmheart::core
