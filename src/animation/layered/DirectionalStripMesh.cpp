#include "animation/layered/DirectionalStripMesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace realmheart::animation::layered {
namespace {

void set_error(std::string* target, std::string message) {
    if (target != nullptr) *target = std::move(message);
}

std::uint32_t read_pixel(const std::uint8_t* address) {
    std::uint32_t pixel = 0;
    std::memcpy(&pixel, address, sizeof(pixel));
    return pixel;
}

double safe_offset(double direction, double displacement, double weight) {
    if (direction == 0.0 || displacement == 0.0 || weight == 0.0) return 0.0;

    const long double result = static_cast<long double>(direction) *
        static_cast<long double>(displacement) * static_cast<long double>(weight);
    if (std::isnan(result)) return 0.0;
    constexpr long double max_double =
        static_cast<long double>(std::numeric_limits<double>::max());
    if (!std::isfinite(result) || result >= max_double) return
        result < 0.0L ? -std::numeric_limits<double>::max() :
        std::numeric_limits<double>::max();
    if (result <= -max_double) return -std::numeric_limits<double>::max();
    return static_cast<double>(result);
}

} // namespace

std::optional<DirectionalStripMesh> DirectionalStripMesh::from_argb32(
    const std::uint8_t* data,
    int width,
    int height,
    int stride,
    int requested_strips,
    StripAxis axis,
    AnchorPolicy anchor,
    Vector2 direction,
    std::string* error_message
) {
    constexpr std::size_t pixel_bytes = sizeof(std::uint32_t);
    if (data == nullptr || width <= 0 || height <= 0 || stride <= 0 ||
        requested_strips < 2 || !std::isfinite(direction.x) ||
        !std::isfinite(direction.y) || std::hypot(direction.x, direction.y) <= 0.0) {
        set_error(error_message, "Directional strip mesh received invalid geometry");
        return std::nullopt;
    }
    const auto width_size = static_cast<std::size_t>(width);
    const auto height_size = static_cast<std::size_t>(height);
    const auto stride_size = static_cast<std::size_t>(stride);
    if (width_size > std::numeric_limits<std::size_t>::max() / pixel_bytes ||
        stride_size < width_size * pixel_bytes ||
        height_size > std::numeric_limits<std::size_t>::max() / stride_size) {
        set_error(error_message, "Directional strip mesh geometry exceeds addressable bounds");
        return std::nullopt;
    }

    DirectionalStripMesh mesh;
    mesh.width_ = width;
    mesh.height_ = height;
    mesh.axis_ = axis;
    mesh.anchor_ = anchor;
    mesh.direction_ = direction;
    const int extent = axis == StripAxis::Rows ? height : width;
    const int strip_count = extent < 2 ? 1 : std::clamp(requested_strips, 2, extent);
    mesh.strips_.reserve(static_cast<std::size_t>(strip_count));

    for (int index = 0; index < strip_count; ++index) {
        const auto extent_size = static_cast<std::uint64_t>(extent);
        const auto index_size = static_cast<std::uint64_t>(index);
        const auto count_size = static_cast<std::uint64_t>(strip_count);
        const int begin = static_cast<int>((extent_size * index_size) / count_size);
        const int end = static_cast<int>((extent_size * (index_size + 1U)) / count_size);
        std::uint64_t color_sum = 0;
        std::uint64_t alpha_sum = 0;
        int visible_min = axis == StripAxis::Rows ? width : height;
        int visible_max = -1;

        const int x0 = axis == StripAxis::Rows ? 0 : begin;
        const int x1 = axis == StripAxis::Rows ? width : end;
        const int y0 = axis == StripAxis::Rows ? begin : 0;
        const int y1 = axis == StripAxis::Rows ? end : height;
        for (int y = y0; y < y1; ++y) {
            const std::uint8_t* row = data +
                (static_cast<std::size_t>(y) * stride_size);
            for (int x = x0; x < x1; ++x) {
                const std::uint32_t pixel = read_pixel(row +
                    (static_cast<std::size_t>(x) * pixel_bytes));
                const std::uint32_t alpha = (pixel >> 24U) & 0xffU;
                if (alpha == 0U) continue;
                const int coordinate = axis == StripAxis::Rows ? x : y;
                visible_min = std::min(visible_min, coordinate);
                visible_max = std::max(visible_max, coordinate);
                color_sum += (pixel >> 16U) & 0xffU;
                alpha_sum += alpha;
            }
        }

        const bool visible = visible_max >= visible_min;
        mesh.strips_.push_back({
            .x = x0,
            .y = y0,
            .width = std::max(x1 - x0, 1),
            .height = std::max(y1 - y0, 1),
            .weight = alpha_sum == 0U ? 0.0 : std::clamp(
                static_cast<double>(color_sum) / static_cast<double>(alpha_sum),
                0.0, 1.0
            ),
            .visible_min = visible ? static_cast<double>(visible_min) : 0.0,
            .visible_max = visible ? static_cast<double>(visible_max + 1) :
                static_cast<double>(axis == StripAxis::Rows ? width : height),
        });
    }

    if (mesh.strips_.size() >= 3U) {
        std::vector<double> smoothed(mesh.strips_.size(), 0.0);
        smoothed.front() = mesh.strips_.front().weight;
        smoothed.back() = mesh.strips_.back().weight;
        for (std::size_t index = 1; index + 1 < mesh.strips_.size(); ++index) {
            smoothed[index] = (mesh.strips_[index - 1].weight +
                (2.0 * mesh.strips_[index].weight) +
                mesh.strips_[index + 1].weight) / 4.0;
        }
        for (std::size_t index = 0; index < mesh.strips_.size(); ++index) {
            mesh.strips_[index].weight = std::clamp(smoothed[index], 0.0, 1.0);
        }
    }
    return mesh;
}

const std::vector<DirectionalStrip>& DirectionalStripMesh::strips() const {
    return strips_;
}

std::vector<DirectionalStripDeformation> DirectionalStripMesh::pose(
    double displacement
) const {
    if (!std::isfinite(displacement)) displacement = 0.0;
    std::vector<DirectionalStripDeformation> result;
    result.reserve(strips_.size());
    for (const auto& strip : strips_) {
        const Vector2 travel{
            .x = safe_offset(direction_.x, displacement, strip.weight),
            .y = safe_offset(direction_.y, displacement, strip.weight),
        };
        DirectionalStripDeformation deformation;
        switch (anchor_) {
        case AnchorPolicy::PinnedMinimum:
            deformation.maximum_offset = travel;
            break;
        case AnchorPolicy::PinnedMaximum:
            deformation.minimum_offset = travel;
            break;
        case AnchorPolicy::WeightedTranslate:
            deformation.minimum_offset = travel;
            deformation.maximum_offset = travel;
            break;
        }
        result.push_back(deformation);
    }
    return result;
}

StripAxis DirectionalStripMesh::axis() const { return axis_; }
AnchorPolicy DirectionalStripMesh::anchor() const { return anchor_; }
Vector2 DirectionalStripMesh::direction() const { return direction_; }
int DirectionalStripMesh::width() const { return width_; }
int DirectionalStripMesh::height() const { return height_; }

} // namespace realmheart::animation::layered
