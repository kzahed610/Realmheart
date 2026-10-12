#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace realmheart::services {

// What a wallpaper discovery sweep produced, and what it had to say about the
// roots it swept.
struct WallpaperDiscovery {
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> diagnostics;
};

// Sweeps one root for wallpapers.
//
// Classification is deliberately cheap: a magic-byte probe for still images, an
// extension for live video, and nothing that decodes pixels. The renderer is the
// final authority on what a source *is*, and it says so at apply time; indexing
// must never decode video content (architecture document section 5.2).
//
// Unreadable, missing, and empty roots are reported as diagnostics rather than
// thrown, so one dead directory never empties the library.
[[nodiscard]] WallpaperDiscovery discover_wallpapers(
    const std::filesystem::path& root
);

// The conventional wallpaper root, used when a setting is empty.
[[nodiscard]] std::filesystem::path default_wallpaper_root();

} // namespace realmheart::services
