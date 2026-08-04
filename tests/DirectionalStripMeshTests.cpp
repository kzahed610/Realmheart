#include "animation/layered/DirectionalStripMesh.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using namespace realmheart::animation::layered;

void write_mask(std::vector<std::uint8_t>& pixels, int stride, int x, int y,
                std::uint8_t gray) {
    const std::uint32_t pixel = 0xff000000U |
        (static_cast<std::uint32_t>(gray) << 16U) |
        (static_cast<std::uint32_t>(gray) << 8U) | gray;
    std::memcpy(pixels.data() + (y * stride) + (x * 4), &pixel, sizeof(pixel));
}

void rows_recover_visible_bounds_and_mask_weights() {
    constexpr int width = 8;
    constexpr int height = 4;
    constexpr int stride = width * 4;
    std::vector<std::uint8_t> pixels(stride * height, 0U);
    for (int y = 0; y < height; ++y) {
        const auto gray = static_cast<std::uint8_t>(80 + (y * 50));
        for (int x = 2; x <= 5; ++x) write_mask(pixels, stride, x, y, gray);
    }

    const auto mesh = DirectionalStripMesh::from_argb32(
        pixels.data(), width, height, stride, 4, StripAxis::Rows,
        AnchorPolicy::PinnedMaximum, {1.0, 0.25}
    );

    assert(mesh.has_value());
    assert(mesh->strips().size() == 4U);
    assert(mesh->strips().front().visible_min == 2.0);
    assert(mesh->strips().front().visible_max == 6.0);
    assert(mesh->strips().front().weight < mesh->strips().back().weight);
    const auto pose = mesh->pose(4.0);
    assert(pose.front().minimum_offset.x > 0.0);
    assert(pose.front().maximum_offset.x == 0.0);
}

void columns_support_minimum_edge_pinning() {
    constexpr int width = 4;
    constexpr int height = 8;
    constexpr int stride = width * 4;
    std::vector<std::uint8_t> pixels(stride * height, 0U);
    for (int x = 0; x < width; ++x) {
        for (int y = 1; y <= 6; ++y) write_mask(pixels, stride, x, y, 255U);
    }

    const auto mesh = DirectionalStripMesh::from_argb32(
        pixels.data(), width, height, stride, 4, StripAxis::Columns,
        AnchorPolicy::PinnedMinimum, {-0.5, 1.0}
    );

    assert(mesh.has_value());
    assert(mesh->strips().front().visible_min == 1.0);
    assert(mesh->strips().front().visible_max == 7.0);
    const auto pose = mesh->pose(2.0);
    assert(pose.front().minimum_offset.x == 0.0);
    assert(std::abs(pose.front().maximum_offset.x + 1.0) < 0.0001);
    assert(std::abs(pose.front().maximum_offset.y - 2.0) < 0.0001);
}

void weighted_translation_moves_both_edges_together() {
    constexpr int stride = 8;
    std::vector<std::uint8_t> pixels(stride * 2, 0U);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) write_mask(pixels, stride, x, y, 255U);
    }
    const auto mesh = DirectionalStripMesh::from_argb32(
        pixels.data(), 2, 2, stride, 2, StripAxis::Rows,
        AnchorPolicy::WeightedTranslate, {0.0, 1.0}
    );
    assert(mesh.has_value());
    const auto pose = mesh->pose(3.0);
    assert(pose.front().minimum_offset.y == pose.front().maximum_offset.y);
    assert(std::abs(pose.front().minimum_offset.y - 3.0) < 0.0001);
}

} // namespace

int main() {
    rows_recover_visible_bounds_and_mask_weights();
    columns_support_minimum_edge_pinning();
    weighted_translation_moves_both_edges_together();
    std::cout << "Directional strip mesh tests passed\n";
    return 0;
}
