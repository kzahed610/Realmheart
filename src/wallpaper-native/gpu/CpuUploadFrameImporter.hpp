#pragma once

#include "wallpaper-native/gpu/RenderFrame.hpp"
#include "wallpaper-native/video/VideoFrame.hpp"

#include <cstdint>
#include <string>

namespace realmheart::wallpaper_native {

// Uploads decoded video frames into reusable GL textures.
//
// The rule from the renderer plan (phase 6): texture storage is allocated only
// when the geometry or layout changes. A same-size frame overwrites with
// glTexSubImage2D, so a long-running wallpaper never re-allocates GPU memory
// per decoded frame.
//
// Frames arrive as NV12 (the shader converts to RGB), so a frame costs one
// luma texture and one chroma texture: 1.5 bytes per pixel uploaded instead of
// 4, and no CPU-side colour conversion at all.
class CpuUploadFrameImporter {
public:
    // Uploads 'source' into 'frame', allocating the texture(s) the first time
    // and on every geometry or format change. Requires a current GL context.
    [[nodiscard]] bool update(
        RenderFrame& frame,
        const VideoFrame& source,
        std::string* error_message
    );

    // Releases the texture(s) held by 'frame'. Requires a current GL context.
    void release(RenderFrame& frame) noexcept;

    [[nodiscard]] std::uint64_t allocations() const noexcept {
        return allocations_;
    }
    [[nodiscard]] std::uint64_t uploads() const noexcept { return uploads_; }

private:
    std::uint64_t allocations_ = 0;
    std::uint64_t uploads_ = 0;
};

} // namespace realmheart::wallpaper_native
