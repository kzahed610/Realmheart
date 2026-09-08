#pragma once

#include "screenshot/SelectionGeometry.hpp"
#include "screenshot/WaylandScreencopy.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace realmheart::screenshot {

// Keep compositor-controlled allocations bounded before they reach memfd, mmap,
// Wayland's signed int API, or a std::vector.
constexpr std::size_t kMaxScreenshotBytes = 512u * 1024u * 1024u;
constexpr std::uint32_t kMaxScreenshotDimension = 32768u;
constexpr std::size_t kMaxOcrStdoutBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxOcrStderrBytes = 64u * 1024u;
constexpr std::size_t kMaxOcrDiagnosticBytes = 4096u;
constexpr std::size_t kMaxOcrWords = 10000u;

struct ScreencopyDimensions {
    std::size_t row_bytes = 0;
    std::size_t total_bytes = 0;
    std::size_t rgba_bytes = 0;
};

inline bool checked_add_size(
    std::size_t left,
    std::size_t right,
    std::size_t& result
) {
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    result = left + right;
    return true;
}

inline bool checked_mul_size(
    std::size_t left,
    std::size_t right,
    std::size_t& result
) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) return false;
    result = left * right;
    return true;
}

inline std::optional<ScreencopyDimensions> validate_screencopy_dimensions(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t stride,
    std::string& error
) {
    error.clear();
    if (
        width == 0 || height == 0 ||
        width > kMaxScreenshotDimension || height > kMaxScreenshotDimension ||
        stride == 0 || stride > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
    ) {
        error = "compositor offered out-of-range screencopy dimensions";
        return std::nullopt;
    }

    std::size_t row_bytes = 0;
    if (!checked_mul_size(static_cast<std::size_t>(width), 4u, row_bytes) ||
        stride < row_bytes) {
        error = "compositor offered an invalid screenshot stride";
        return std::nullopt;
    }

    std::size_t total_bytes = 0;
    std::size_t rgba_bytes = 0;
    if (!checked_mul_size(static_cast<std::size_t>(stride), height, total_bytes) ||
        !checked_mul_size(row_bytes, height, rgba_bytes) ||
        total_bytes > kMaxScreenshotBytes || rgba_bytes > kMaxScreenshotBytes ||
        total_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "compositor screenshot exceeds the bounded frame size";
        return std::nullopt;
    }

    return ScreencopyDimensions{
        .row_bytes = row_bytes,
        .total_bytes = total_bytes,
        .rgba_bytes = rgba_bytes,
    };
}

inline bool validate_pixel_rect(
    const FrozenFrame& frame,
    const PixelRect& region,
    std::string& error
) {
    error.clear();
    if (
        frame.width <= 0 || frame.height <= 0 || frame.stride <= 0 ||
        frame.rgba.empty()
    ) {
        error = "frozen frame is invalid";
        return false;
    }
    if (
        region.x < 0 || region.y < 0 ||
        region.width <= 0 || region.height <= 0
    ) {
        error = "selection is empty or negative";
        return false;
    }

    const std::int64_t right = static_cast<std::int64_t>(region.x) + region.width;
    const std::int64_t bottom = static_cast<std::int64_t>(region.y) + region.height;
    std::size_t frame_row_bytes = 0;
    if (!checked_mul_size(static_cast<std::size_t>(frame.width), 4u, frame_row_bytes) ||
        static_cast<std::size_t>(frame.stride) < frame_row_bytes) {
        error = "frozen frame stride is invalid";
        return false;
    }

    if (
        right > frame.width || bottom > frame.height ||
        right < 0 || bottom < 0
    ) {
        error = "selection is outside the frozen frame";
        return false;
    }

    std::size_t frame_bytes = 0;
    std::size_t required_bytes = 0;
    std::size_t crop_row_bytes = 0;
    std::size_t crop_bytes = 0;
    if (
        !checked_mul_size(static_cast<std::size_t>(frame.stride), frame.height, frame_bytes) ||
        !checked_mul_size(static_cast<std::size_t>(frame.width), 4u, required_bytes) ||
        !checked_mul_size(static_cast<std::size_t>(region.width), 4u, crop_row_bytes) ||
        !checked_mul_size(crop_row_bytes, static_cast<std::size_t>(region.height), crop_bytes) ||
        frame_bytes > kMaxScreenshotBytes || required_bytes > kMaxScreenshotBytes ||
        crop_bytes > kMaxScreenshotBytes || frame.rgba.size() < frame_bytes
    ) {
        error = "screenshot buffer arithmetic exceeds the bounded frame size";
        return false;
    }
    return true;
}

} // namespace realmheart::screenshot
