#pragma once

#include "wallpaper-native/gpu/RenderFrame.hpp"

#include <GLES2/gl2.h>

#include <array>
#include <string>

namespace realmheart::wallpaper_native {

// The wallpaper crossfade pass.
//
// Draws the current frame (or blends it into an incoming one) over a full-screen
// triangle strip, cropping each frame to cover the output. Both frames may be
// RGB or YUV: the conversion matrix rides along with the frame, so a static
// wallpaper, a live one, and any transition between them are the same draw call
// (renderer docs section 10.4).
//
// Extracted out of NativeWallpaperRenderer so the conversion can be exercised
// headlessly against known pixels.
class WallpaperShaders {
public:
    WallpaperShaders() = default;
    ~WallpaperShaders();

    WallpaperShaders(const WallpaperShaders&) = delete;
    WallpaperShaders& operator=(const WallpaperShaders&) = delete;

    // Compiles and links the program. Returns false with a reason on failure.
    [[nodiscard]] bool initialize(std::string* error_message = nullptr);
    void release() noexcept;
    [[nodiscard]] bool valid() const noexcept { return program_ != 0; }

    // Binds both frames, sets the crop and progress uniforms, and draws. An
    // invalid 'incoming' means there is no transition: both samplers read the
    // current frame and progress is forced to 1.
    void draw(
        const RenderFrameView& current,
        const RenderFrameView& incoming,
        std::array<float, 4> current_crop,
        std::array<float, 4> incoming_crop,
        float progress
    ) const noexcept;

private:
    GLuint program_ = 0;
    GLint position_attribute_ = -1;
    GLint uv_attribute_ = -1;
    GLint current_sampler_ = -1;
    GLint next_sampler_ = -1;
    GLint current_chroma_sampler_ = -1;
    GLint next_chroma_sampler_ = -1;
    GLint current_is_yuv_ = -1;
    GLint next_is_yuv_ = -1;
    GLint progress_uniform_ = -1;
    GLint current_crop_uniform_ = -1;
    GLint next_crop_uniform_ = -1;
    GLint current_yuv_matrix_ = -1;
    GLint next_yuv_matrix_ = -1;
    GLint current_yuv_offset_ = -1;
    GLint next_yuv_offset_ = -1;
};

} // namespace realmheart::wallpaper_native
