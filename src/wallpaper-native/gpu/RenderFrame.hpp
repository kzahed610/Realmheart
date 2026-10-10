#pragma once

#include <GLES2/gl2.h>

#include <cstdint>

namespace realmheart::wallpaper_native {

// How a frame's pixels reach the shader.
//
// Only RGB textures exist today. The enum exists so hardware decode and
// imported YUV/external-image planes (renderer docs sections 5.2 and 10.4)
// can arrive without reworking every draw call site.
enum class RenderFrameKind {
    RgbTexture,
};

// A renderable frame. The renderer may ask only for its kind, geometry, and
// how to bind it; never for how it was produced.
struct RenderFrame {
    RenderFrameKind kind = RenderFrameKind::RgbTexture;
    GLuint texture = 0;
    int width = 0;
    int height = 0;
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
    [[nodiscard]] int width() const noexcept { return valid() ? frame->width : 0; }
    [[nodiscard]] int height() const noexcept {
        return valid() ? frame->height : 0;
    }
    [[nodiscard]] RenderFrameKind kind() const noexcept {
        return frame != nullptr ? frame->kind : RenderFrameKind::RgbTexture;
    }
};

} // namespace realmheart::wallpaper_native
