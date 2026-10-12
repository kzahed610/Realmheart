#pragma once

#include "wallpaper-native/gpu/YuvColorimetry.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace realmheart::wallpaper_native {

// How the pixels in a decoded frame are laid out.
//
// NV12 is the working format: it costs a third of the upload bandwidth of RGBA
// and skips the RGB conversion entirely, because the YUV-to-RGB matrix runs in
// the fragment shader instead (measured on the host: 1.03 ms/frame to produce
// against 2.95 ms for RGBA, and 3.11 MB uploaded instead of 8.29 MB).
enum class VideoFrameFormat {
    Rgba8,
    Nv12,
};

// One decoded frame. Self-contained so its lifetime is independent of the
// decoder that produced it and of any FFmpeg allocation.
struct VideoFrame {
    VideoFrameFormat format = VideoFrameFormat::Rgba8;
    int width = 0;
    int height = 0;
    // Presentation time relative to playback start, in microseconds. A frame
    // without a usable timestamp carries has_timestamp = false and leaves the
    // fallback policy to the frame scheduler (renderer docs section 8.2).
    std::int64_t presentation_us = 0;
    bool has_timestamp = false;
    VideoColorSpace color_space = VideoColorSpace::Bt601;
    VideoColorRange color_range = VideoColorRange::Limited;

    // Rgba8: tightly packed RGBA8, width * height * 4 bytes.
    // Nv12: the Y plane (width * height) immediately followed by the
    // interleaved UV plane (width * height / 2 bytes, one U,V pair per 2x2
    // block).
    std::vector<unsigned char> pixels;

    [[nodiscard]] bool valid() const noexcept {
        if (width <= 0 || height <= 0) return false;
        const auto expected = static_cast<std::size_t>(width) *
                              static_cast<std::size_t>(height);
        if (format == VideoFrameFormat::Nv12) {
            return pixels.size() == expected + expected / 2;
        }
        return pixels.size() == expected * 4U;
    }

    // Byte offset of the chroma plane inside 'pixels' (Nv12 only).
    [[nodiscard]] std::size_t plane_offset() const noexcept {
        return static_cast<std::size_t>(width) *
               static_cast<std::size_t>(height);
    }
};

} // namespace realmheart::wallpaper_native
