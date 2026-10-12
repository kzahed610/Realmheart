#pragma once

namespace realmheart::wallpaper_native {

// Colour metadata that travels with a frame.
//
// A wallpaper must look the same as it did through the previous RGBA path, so
// the conversion matrix is chosen from what the decoder says about the frame
// rather than assumed. BT.601 limited range is the default because it is what
// FFmpeg assumes for untagged content.
enum class VideoColorSpace {
    Bt601,
    Bt709,
};

enum class VideoColorRange {
    Limited,
    Full,
};

// The YUV-to-RGB transform for one frame, in the shader's terms:
// rgb = matrix * (y + luma_offset, u - 0.5, v - 0.5), clamped.
struct YuvConversion {
    // Row-major 3x3.
    float matrix[9] = {
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F
    };
    // What luma has to lose before the matrix applies: the 16/235 lift of
    // limited-range video. Full-range luma is already at zero.
    float luma_offset = 0.0F;
};

// The transform for one colourimetry pair.
[[nodiscard]] YuvConversion yuv_conversion_for(
    VideoColorSpace color_space,
    VideoColorRange color_range
) noexcept;

} // namespace realmheart::wallpaper_native
