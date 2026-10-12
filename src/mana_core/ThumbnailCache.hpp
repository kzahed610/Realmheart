#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <filesystem>
#include <string>

namespace realmheart::mana_core {

class ThumbnailCache {
public:
    // Device-resolution covers are bounded by the decoder's source-dimension cap.
    [[nodiscard]] static constexpr int max_preview_dimension() noexcept {
        return 16'384;
    }

    // True for extensions the live-wallpaper path can decode. This decides that
    // a poster frame is needed instead of a still image, so the selector uses
    // the same predicate to size that poster rather than keeping a second,
    // drifting copy of the list.
    [[nodiscard]] static bool is_live_wallpaper(
        const std::filesystem::path& source_path
    ) noexcept;

    // Load a cached thumbnail for the given wallpaper path.
    // Falls back to decoding it using gdk_pixbuf_new_from_file_at_scale (with aspect ratio preservation)
    // and stores the result in the cache for future reads.
    [[nodiscard]] static GdkPixbuf* load_or_create(
        const std::filesystem::path& source_path,
        int target_dimension,
        std::string* error = nullptr
    );
};

} // namespace realmheart::mana_core
