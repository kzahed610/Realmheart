#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <filesystem>
#include <string>

namespace realmheart::mana_core {

class ThumbnailCache {
public:
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
