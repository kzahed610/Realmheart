#include "animation/character/HairFlowWarp.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using realmheart::animation::character::Argb32ImageView;
using realmheart::animation::character::warp_hair_argb32;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::uint32_t argb(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (static_cast<std::uint32_t>(a) << 24U) |
        (static_cast<std::uint32_t>(r) << 16U) |
        (static_cast<std::uint32_t>(g) << 8U) |
        static_cast<std::uint32_t>(b);
}

std::vector<std::uint8_t> words(std::initializer_list<std::uint32_t> pixels) {
    std::vector<std::uint8_t> result(pixels.size() * 4U);
    std::size_t offset = 0;
    for (const auto pixel : pixels) {
        std::memcpy(result.data() + offset, &pixel, sizeof(pixel));
        offset += 4U;
    }
    return result;
}

std::uint32_t read_word(const std::vector<std::uint8_t>& bytes, std::size_t index) {
    std::uint32_t word = 0;
    std::memcpy(&word, bytes.data() + index * 4U, sizeof(word));
    return word;
}

} // namespace

int main() {
    const auto source = words({
        argb(255, 255, 0, 0),
        argb(255, 0, 255, 0),
        argb(255, 0, 0, 255),
    });
    // Encoded +X direction: R=255, G=128. Full alpha means premultiplied
    // storage is identical to the authored channels.
    const auto flow = words({
        argb(255, 255, 128, 128),
        argb(255, 255, 128, 128),
        argb(255, 255, 128, 128),
    });
    const auto mask = words({
        argb(255, 255, 255, 255),
        argb(255, 255, 255, 255),
        argb(255, 255, 255, 255),
    });
    const Argb32ImageView source_view{source.data(), 3, 1, 12};
    const Argb32ImageView flow_view{flow.data(), 3, 1, 12};
    const Argb32ImageView mask_view{mask.data(), 3, 1, 12};

    std::string error;
    const auto unchanged = warp_hair_argb32(
        source_view, flow_view, mask_view, 0.0, &error
    );
    require(unchanged.has_value(), "zero warp must succeed: " + error);
    require(*unchanged == source, "zero displacement must preserve exact pixels");

    const auto shifted = warp_hair_argb32(
        source_view, flow_view, mask_view, 1.0, &error
    );
    require(shifted.has_value(), "positive warp must succeed: " + error);
    require(read_word(*shifted, 1) == argb(255, 255, 0, 0),
            "inverse sampling must move the first source pixel toward +X");

    const auto pinned_mask = words({
        argb(255, 0, 0, 0),
        argb(255, 0, 0, 0),
        argb(255, 0, 0, 0),
    });
    const Argb32ImageView pinned_view{pinned_mask.data(), 3, 1, 12};
    const auto pinned = warp_hair_argb32(
        source_view, flow_view, pinned_view, 2.0, &error
    );
    require(pinned.has_value() && *pinned == source,
            "black movement mask must pin the source exactly");

    std::cout << "Hair flow warp tests passed\n";
    return 0;
}
