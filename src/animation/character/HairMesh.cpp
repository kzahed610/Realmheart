#include "animation/character/HairMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace realmheart::animation::character {
namespace {

void set_error(std::string* target, std::string message) {
    if (target != nullptr) *target = std::move(message);
}

std::uint32_t read_pixel(const unsigned char* address) {
    std::uint32_t pixel = 0;
    std::memcpy(&pixel, address, sizeof(pixel));
    return pixel;
}

} // namespace

std::optional<HairMesh> HairMesh::from_argb32(
    const unsigned char* data,
    int width,
    int height,
    int stride,
    int requested_rows,
    std::string* error_message
) {
    if (data == nullptr || width <= 0 || height <= 0 ||
        stride < width * static_cast<int>(sizeof(std::uint32_t)) ||
        requested_rows < 2) {
        set_error(error_message, "Hair mesh received invalid mask geometry");
        return std::nullopt;
    }

    HairMesh mesh;
    mesh.width_ = width;
    mesh.height_ = height;

    const int rows = std::clamp(requested_rows, 2, height);
    mesh.bands_.reserve(static_cast<std::size_t>(rows));

    for (int index = 0; index < rows; ++index) {
        const int y0 = (height * index) / rows;
        const int y1 = (height * (index + 1)) / rows;
        std::uint64_t premultiplied_red_sum = 0;
        std::uint64_t alpha_sum = 0;
        int visible_outer_x = width;
        int visible_inner_x = -1;

        for (int y = y0; y < y1; ++y) {
            const unsigned char* row = data + (y * stride);
            for (int x = 0; x < width; ++x) {
                const std::uint32_t pixel = read_pixel(
                    row + (x * static_cast<int>(sizeof(std::uint32_t)))
                );
                const std::uint32_t alpha = (pixel >> 24U) & 0xffU;
                if (alpha == 0U) continue;

                visible_outer_x = std::min(visible_outer_x, x);
                visible_inner_x = std::max(visible_inner_x, x);

                // Cairo ARGB32 stores premultiplied channels. Dividing the
                // accumulated red channel by accumulated alpha recovers the
                // grayscale movement strength without transparent padding
                // diluting the result.
                const std::uint32_t red = (pixel >> 16U) & 0xffU;
                premultiplied_red_sum += red;
                alpha_sum += alpha;
            }
        }

        const double weight = alpha_sum == 0U
            ? 0.0
            : std::clamp(
                static_cast<double>(premultiplied_red_sum) /
                    static_cast<double>(alpha_sum),
                0.0,
                1.0
            );
        const bool has_visible_pixels = visible_inner_x >= visible_outer_x;
        mesh.bands_.push_back({
            .y = y0,
            .height = std::max(y1 - y0, 1),
            .weight = weight,
            .outer_x = has_visible_pixels
                ? static_cast<double>(visible_outer_x)
                : 0.0,
            .inner_x = has_visible_pixels
                ? static_cast<double>(visible_inner_x + 1)
                : static_cast<double>(width),
        });
    }

    // One restrained smoothing pass prevents visible step changes between
    // adjacent bands while preserving the mask's pinned-root gradient.
    if (mesh.bands_.size() >= 3U) {
        std::vector<double> smoothed(mesh.bands_.size(), 0.0);
        smoothed.front() = mesh.bands_.front().weight;
        smoothed.back() = mesh.bands_.back().weight;
        for (std::size_t index = 1; index + 1 < mesh.bands_.size(); ++index) {
            smoothed[index] =
                (mesh.bands_[index - 1].weight +
                 (2.0 * mesh.bands_[index].weight) +
                 mesh.bands_[index + 1].weight) /
                4.0;
        }
        for (std::size_t index = 0; index < mesh.bands_.size(); ++index) {
            mesh.bands_[index].weight = std::clamp(smoothed[index], 0.0, 1.0);
        }
    }

    return mesh;
}

} // namespace realmheart::animation::character
