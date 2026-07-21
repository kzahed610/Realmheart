#include "animation/character/HairMesh.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using realmheart::animation::character::HairMesh;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void write_argb32(
    std::vector<unsigned char>& pixels,
    int stride,
    int x,
    int y,
    std::uint8_t gray,
    std::uint8_t alpha = 255U
) {
    const std::uint32_t premultiplied =
        (static_cast<std::uint32_t>(gray) * alpha + 127U) / 255U;
    const std::uint32_t pixel =
        (static_cast<std::uint32_t>(alpha) << 24U) |
        (premultiplied << 16U) |
        (premultiplied << 8U) |
        premultiplied;
    std::memcpy(
        pixels.data() + (y * stride) +
            (x * static_cast<int>(sizeof(std::uint32_t))),
        &pixel,
        sizeof(pixel)
    );
}

void test_mask_builds_pinned_to_free_gradient() {
    constexpr int width = 4;
    constexpr int height = 8;
    constexpr int stride = width * static_cast<int>(sizeof(std::uint32_t));
    std::vector<unsigned char> pixels(
        static_cast<std::size_t>(stride * height),
        0U
    );

    for (int y = 0; y < height; ++y) {
        const auto gray = static_cast<std::uint8_t>(
            (255 * y) / (height - 1)
        );
        for (int x = 0; x < width; ++x) {
            write_argb32(pixels, stride, x, y, gray);
        }
    }

    std::string error;
    auto mesh = HairMesh::from_argb32(
        pixels.data(), width, height, stride, 4, &error
    );
    require(mesh.has_value(), "synthetic gradient mask must build: " + error);
    require(mesh->bands().size() == 4U, "requested strip count must be preserved");
    require(mesh->bands().front().weight < 0.15,
            "root band must remain almost pinned");
    require(mesh->bands().back().weight > 0.80,
            "tip band must retain strong movement");
    for (std::size_t index = 1; index < mesh->bands().size(); ++index) {
        require(mesh->bands()[index].weight >= mesh->bands()[index - 1].weight,
                "smoothed mask weights must remain monotonic");
    }
}

void test_transparent_padding_does_not_dilute_visible_weight() {
    constexpr int width = 4;
    constexpr int height = 4;
    constexpr int stride = width * static_cast<int>(sizeof(std::uint32_t));
    std::vector<unsigned char> pixels(
        static_cast<std::size_t>(stride * height),
        0U
    );

    for (int y = 0; y < height; ++y) {
        write_argb32(pixels, stride, 1, y, 255U);
    }

    auto mesh = HairMesh::from_argb32(
        pixels.data(), width, height, stride, 2
    );
    require(mesh.has_value(), "sparse opaque mask must build");
    for (const auto& band : mesh->bands()) {
        require(std::abs(band.weight - 1.0) < 0.0001,
                "transparent pixels must not weaken white movement pixels");
    }
}

void test_visible_bounds_ignore_transparent_canvas_padding() {
    constexpr int width = 8;
    constexpr int height = 4;
    constexpr int stride = width * static_cast<int>(sizeof(std::uint32_t));
    std::vector<unsigned char> pixels(
        static_cast<std::size_t>(stride * height),
        0U
    );

    for (int y = 0; y < height; ++y) {
        for (int x = 2; x <= 5; ++x) {
            write_argb32(pixels, stride, x, y, 255U);
        }
    }

    auto mesh = HairMesh::from_argb32(
        pixels.data(), width, height, stride, 2
    );
    require(mesh.has_value(), "padded mask must build");
    for (const auto& band : mesh->bands()) {
        require(std::abs(band.outer_x - 2.0) < 0.0001,
                "mesh must recover the first visible pixel, not canvas x=0");
        require(std::abs(band.inner_x - 6.0) < 0.0001,
                "mesh must pin the visible inner edge, not the padded canvas edge");
    }
}

} // namespace

int main() {
    test_mask_builds_pinned_to_free_gradient();
    test_transparent_padding_does_not_dilute_visible_weight();
    test_visible_bounds_ignore_transparent_canvas_padding();
    std::cout << "Hair mesh tests passed\n";
    return 0;
}
