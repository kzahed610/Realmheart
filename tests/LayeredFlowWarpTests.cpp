#include "animation/layered/FlowWarp.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using realmheart::animation::layered::Argb32ImageView;
using realmheart::animation::layered::warp_argb32;

void write_pixel(std::vector<std::uint8_t>& pixels, int x, std::uint32_t value) {
    std::memcpy(pixels.data() + static_cast<std::size_t>(x * 4), &value, sizeof(value));
}

std::uint32_t read_pixel(const std::vector<std::uint8_t>& pixels, int x) {
    std::uint32_t value = 0;
    std::memcpy(&value, pixels.data() + static_cast<std::size_t>(x * 4), sizeof(value));
    return value;
}

void zero_displacement_preserves_source() {
    constexpr int width = 3;
    constexpr int stride = width * 4;
    std::vector<std::uint8_t> source(stride, 0U);
    std::vector<std::uint8_t> flow(stride, 0U);
    std::vector<std::uint8_t> mask(stride, 0U);
    write_pixel(source, 0, 0xffff0000U);
    write_pixel(source, 1, 0xff00ff00U);
    write_pixel(source, 2, 0xff0000ffU);
    for (int x = 0; x < width; ++x) {
        write_pixel(flow, x, 0xff8080ffU);
        write_pixel(mask, x, 0xffffffffU);
    }

    const Argb32ImageView source_view{source.data(), width, 1, stride};
    const Argb32ImageView flow_view{flow.data(), width, 1, stride};
    const Argb32ImageView mask_view{mask.data(), width, 1, stride};
    const auto warped = warp_argb32(source_view, flow_view, mask_view, 0.0);

    assert(warped.has_value());
    assert(*warped == source);
}

void directional_displacement_inverse_maps_pixels() {
    constexpr int width = 3;
    constexpr int stride = width * 4;
    std::vector<std::uint8_t> source(stride, 0U);
    std::vector<std::uint8_t> flow(stride, 0U);
    std::vector<std::uint8_t> mask(stride, 0U);
    write_pixel(source, 0, 0xffff0000U);
    write_pixel(source, 1, 0xff00ff00U);
    write_pixel(source, 2, 0xff0000ffU);
    for (int x = 0; x < width; ++x) {
        write_pixel(flow, x, 0xffff80ffU);
        write_pixel(mask, x, 0xffffffffU);
    }

    const auto warped = warp_argb32(
        {source.data(), width, 1, stride},
        {flow.data(), width, 1, stride},
        {mask.data(), width, 1, stride},
        1.0
    );

    assert(warped.has_value());
    assert(read_pixel(*warped, 1) == 0xffff0000U);
    assert(read_pixel(*warped, 2) == 0xff00ff00U);
}

} // namespace

int main() {
    zero_displacement_preserves_source();
    directional_displacement_inverse_maps_pixels();
    std::cout << "Layered flow warp tests passed\n";
    return 0;
}
