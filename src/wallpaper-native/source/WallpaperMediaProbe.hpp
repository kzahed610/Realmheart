#pragma once

#include <string>
#include <string_view>

namespace realmheart::wallpaper_native {

// How a source should be handled. Media type belongs to the renderer layer;
// the transport type in src/services/WallpaperSource.hpp (path vs owned bytes)
// stays orthogonal to it.
enum class WallpaperMediaType {
    // GdkPixbuf can decode it: the static image path, unchanged.
    StaticImage,
    // FFmpeg can demux a video stream from it: the live path.
    Video,
    // Neither: a clear source error, current wallpaper untouched.
    Unsupported,
};

// Result of classifying a source without decoding pixels.
struct WallpaperMediaProbe {
    WallpaperMediaType type = WallpaperMediaType::Unsupported;
    // Human-readable summary for logs and the capability line, e.g.
    // "H.264 1920x1080 30fps". Empty for StaticImage/Unsupported.
    std::string detail;
    // Why the source was rejected. Empty unless type is Unsupported.
    std::string reason;
};

// Classifies an external path. Order matters and is deliberate:
//   1. readability and the shared size budget;
//   2. a cheap GdkPixbuf image probe (an image wins, including animation
//      formats whose first frame GdkPixbuf can decode);
//   3. otherwise an FFmpeg probe for a video stream.
// Extensions are never the authority: a mislabelled file classifies by
// content, exactly as the renderer docs section 6 requires.
[[nodiscard]] WallpaperMediaProbe probe_wallpaper_media(const std::string& path);

// Same classification for owned bytes, but only to answer one question: does
// this payload contain a video stream? Owned video payloads stay unsupported
// by contract (renderer docs section 13.4), so a Video verdict tells the caller
// to reject it with a specific message. A non-video payload is Unsupported
// here, and the image decoder keeps the final say on whether it is an image.
[[nodiscard]] WallpaperMediaProbe probe_wallpaper_media_bytes(
    std::string_view bytes
);

} // namespace realmheart::wallpaper_native
