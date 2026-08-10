#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace realmheart::relictombs {

struct WallpaperDiscovery {
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> diagnostics;
};

class WallpaperLibrary {
public:
    [[nodiscard]] static std::filesystem::path default_root();
    [[nodiscard]] WallpaperDiscovery discover(
        std::filesystem::path root = {}
    ) const;
};

} // namespace realmheart::relictombs
