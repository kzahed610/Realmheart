#include "mana_core/WallpaperLibrary.hpp"
#include "services/WallpaperDiscovery.hpp"

namespace realmheart::mana_core {

// The selector's wallpaper discovery lives in the service layer now, so
// rotation can index exactly the same roots the selector shows. This stays as
// the seam the selector already calls.
std::filesystem::path WallpaperLibrary::default_root() {
    return services::default_wallpaper_root();
}

WallpaperDiscovery WallpaperLibrary::discover(std::filesystem::path root) const {
    return services::discover_wallpapers(root.empty() ? default_root() : root);
}

} // namespace realmheart::mana_core
