#pragma once

#include "wallpaper-native/gpu/RenderFrame.hpp"
#include "wallpaper-native/video/VideoFrame.hpp"

#include <cstdint>
#include <string>

namespace realmheart::wallpaper_native {

// Uploads CPU-decoded video frames into reusable GL textures.
//
// The rule from the renderer plan (phase 6): texture storage is allocated only
// when the geometry changes. A same-size frame overwrites with
// glTexSubImage2D, so a long-running wallpaper never re-allocates GPU memory
// per decoded frame.
class CpuUploadFrameImporter {
public:
    // Uploads 'source' into 'frame', allocating the texture the first time and
    // on every geometry change. Requires a current GL context.
    [[nodiscard]] bool update(
        RenderFrame& frame,
        const VideoFrame& source,
        std::string* error_message
    );

    // Releases the texture held by 'frame'. Requires a current GL context.
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
