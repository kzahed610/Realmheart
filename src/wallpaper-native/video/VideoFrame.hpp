#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace realmheart::wallpaper_native {

// How the pixels in a decoded frame are laid out.
//
// Only CPU-side RGBA exists today: the software MVP. Hardware decode and
// imported DMA-BUF planes arrive later behind the same boundary, so no
// AVFrame* ever crosses into the renderer (renderer docs section 5.2).
enum class VideoFrameFormat {
    Rgba8,
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
    // Tightly packed RGBA8, width * height * 4 bytes.
    std::vector<unsigned char> pixels;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<std::size_t>(width) *
                                    static_cast<std::size_t>(height) * 4U;
    }
};

} // namespace realmheart::wallpaper_native
