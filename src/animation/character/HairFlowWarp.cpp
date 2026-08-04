#include "animation/character/HairFlowWarp.hpp"

namespace realmheart::animation::character {

std::optional<std::vector<std::uint8_t>> warp_hair_argb32(
    Argb32ImageView source,
    Argb32ImageView flow,
    Argb32ImageView movement_mask,
    double displacement_pixels,
    std::string* error_message
) {
    return layered::warp_argb32(
        source, flow, movement_mask, displacement_pixels, error_message
    );
}

} // namespace realmheart::animation::character
