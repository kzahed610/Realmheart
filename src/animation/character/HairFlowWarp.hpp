#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace realmheart::animation::character {

struct Argb32ImageView {
    const std::uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
};

// Produces premultiplied Cairo ARGB32 pixels. The flow map stores encoded
// direction in R/G and the movement mask controls local strength. Sampling is
// inverse-mapped with bilinear filtering so transparent boundaries remain
// stable and the source never develops unfilled holes.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> warp_hair_argb32(
    Argb32ImageView source,
    Argb32ImageView flow,
    Argb32ImageView movement_mask,
    double displacement_pixels,
    std::string* error_message = nullptr
);

} // namespace realmheart::animation::character
