#pragma once

#include "wallpaper-native/gpu/YuvColorimetry.hpp"
#include "wallpaper-native/video/VideoFrame.hpp"

#include <GLES2/gl2.h>

#include <cstdint>

namespace realmheart::wallpaper_native {

// How a frame's pixels reach the shader.
//
// RgbTexture is a static image (or the RGBA fallback). Nv12Planes is a decoded
// video frame: luma in one texture, interleaved chroma in a second, converted
// to RGB in the shader. The enum exists so hardware decode and imported
// DMA-BUF planes (renderer docs sections 5.2 and 10.4) can arrive without
// reworking every draw call site.
enum class RenderFrameKind {
    RgbTexture,
    Nv12Planes,
};

// A renderable frame. The renderer may ask only for its kind, geometry, how to
// bind it, and what colour conversion it needs; never for how it was produced.
struct RenderFrame {
    RenderFrameKind kind = RenderFrameKind::RgbTexture;
    GLuint texture = 0;
    // Nv12Planes only: the interleaved chroma plane, half the luma resolution.
    GLuint chroma_texture = 0;
    int width = 0;
    int height = 0;
    // Nv12Planes only: what the frame needs to become RGB.
    VideoColorSpace color_space = VideoColorSpace::Bt601;
    VideoColorRange color_range = VideoColorRange::Limited;
};

// Non-owning view of a live frame, valid while the owning source publishes it.
struct RenderFrameView {
    const RenderFrame* frame = nullptr;

    [[nodiscard]] bool valid() const noexcept {
        return frame != nullptr && frame->texture != 0;
    }

    [[nodiscard]] GLuint texture() const noexcept {
        return valid() ? frame->texture : 0;
    }

    [[nodiscard]] GLuint chroma_texture() const noexcept {
        return (valid() && frame->kind == RenderFrameKind::Nv12Planes)
            ? frame->chroma_texture
            : 0;
    }

    [[nodiscard]] int width() const noexcept { return valid() ? frame->width : 0; }

    [[nodiscard]] int height() const noexcept {
        return valid() ? frame->height : 0;
    }

    [[nodiscard]] RenderFrameKind kind() const noexcept {
        return frame != nullptr ? frame->kind : RenderFrameKind::RgbTexture;
    }

    // The colour conversion a YUV frame needs. Meaningless for an RGB frame.
    [[nodiscard]] VideoColorSpace color_space() const noexcept {
        return frame != nullptr ? frame->color_space : VideoColorSpace::Bt601;
    }

    [[nodiscard]] VideoColorRange color_range() const noexcept {
        return frame != nullptr ? frame->color_range : VideoColorRange::Limited;
    }
};

// The render-frame kind a decoded frame's format needs.
[[nodiscard]] constexpr RenderFrameKind render_frame_kind_for(
    VideoFrameFormat format
) noexcept {
    return format == VideoFrameFormat::Nv12
        ? RenderFrameKind::Nv12Planes
        : RenderFrameKind::RgbTexture;
}

} // namespace realmheart::wallpaper_native
