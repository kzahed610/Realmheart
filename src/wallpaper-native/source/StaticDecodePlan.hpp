#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace realmheart::wallpaper_native {

// Upper bound on decoded static pixels. Static sources are never upscaled, so
// this bounds the worst-case texture and decode buffer for one wallpaper.
inline constexpr std::uintmax_t kMaxStaticDecodedPixels = 64'000'000;

// Resolves the pixel geometry a static wallpaper is decoded at.
//
// The decision is pure so the renderer and the tests share one contract:
// decode geometry never depends on monitor refresh rate, GL state, or FFmpeg.
struct StaticDecodePlan {
    bool valid = false;
    int source_width = 0;
    int source_height = 0;
    int target_width = 0;
    int target_height = 0;
    // True when the decoded geometry differs from the source, i.e. the caller
    // must decode scaled rather than at native resolution.
    bool downscaled = false;
};

// Decode geometry is intentionally source-native: a wallpaper is decoded at
// its full source resolution so a later hotplug or scale increase never
// stretches a texture that was chosen for an older output topology. Output
// topology therefore decides *whether* a source is re-decoded (through
// native_output_requires_redecode), never *how small* it is decoded.
//
// maximum_texture_size <= 0 means the GL limit is unknown and is not applied.
[[nodiscard]] inline StaticDecodePlan plan_static_decode(
    int source_width,
    int source_height,
    int maximum_texture_size
) noexcept {
    StaticDecodePlan plan;
    plan.source_width = source_width;
    plan.source_height = source_height;
    if (source_width <= 0 || source_height <= 0) return plan;

    if (static_cast<std::uintmax_t>(source_width) >
            kMaxStaticDecodedPixels / static_cast<std::uintmax_t>(source_height)) {
        return plan;
    }

    int target_width = source_width;
    int target_height = source_height;
    if (maximum_texture_size > 0 &&
        (target_width > maximum_texture_size || target_height > maximum_texture_size)) {
        const double limit_scale = std::min(
            static_cast<double>(maximum_texture_size) /
                static_cast<double>(target_width),
            static_cast<double>(maximum_texture_size) /
                static_cast<double>(target_height)
        );
        target_width = std::max(
            1,
            static_cast<int>(
                std::floor(static_cast<double>(target_width) * limit_scale)
            )
        );
        target_height = std::max(
            1,
            static_cast<int>(
                std::floor(static_cast<double>(target_height) * limit_scale)
            )
        );
    }

    plan.valid = true;
    plan.target_width = target_width;
    plan.target_height = target_height;
    plan.downscaled =
        target_width != source_width || target_height != source_height;
    return plan;
}

} // namespace realmheart::wallpaper_native
