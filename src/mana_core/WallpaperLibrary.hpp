#pragma once

#include "services/WallpaperDiscovery.hpp"

#include <filesystem>

namespace realmheart::mana_core {

// The selector's discovery type is the service one: the selector and wallpaper
// rotation index exactly the same walls, so there is one shape for what a sweep
// found.
using WallpaperDiscovery = realmheart::services::WallpaperDiscovery;

class WallpaperLibrary {
public:
    [[nodiscard]] static std::filesystem::path default_root();
    [[nodiscard]] WallpaperDiscovery discover(
        std::filesystem::path root = {}
    ) const;
};

} // namespace realmheart::mana_core
