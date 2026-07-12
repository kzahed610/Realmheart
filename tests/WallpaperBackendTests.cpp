#include "ui/wallpaper/WallpaperBackend.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

} // namespace

int main() {
    using realmheart::ui::wallpaper::WallpaperBackendType;
    using realmheart::ui::wallpaper::parse_wallpaper_backend_type;
    using realmheart::ui::wallpaper::wallpaper_backend_type_name;

    try {
        require(parse_wallpaper_backend_type("gtk") == WallpaperBackendType::Gtk,
                "gtk backend should parse");
        require(parse_wallpaper_backend_type("gtk4") == WallpaperBackendType::Gtk,
                "gtk4 alias should parse");
        require(parse_wallpaper_backend_type("native") == WallpaperBackendType::Native,
                "native backend should parse");
        require(parse_wallpaper_backend_type("wayland") == WallpaperBackendType::Native,
                "wayland alias should parse");
        require(!parse_wallpaper_backend_type("electron"),
                "unknown backend should be rejected with prejudice");
        require(wallpaper_backend_type_name(WallpaperBackendType::Gtk) == "gtk",
                "GTK backend name should be stable");
        require(wallpaper_backend_type_name(WallpaperBackendType::Native) == "native",
                "native backend name should be stable");
    } catch (const std::exception& error) {
        std::cerr << "WallpaperBackendTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "WallpaperBackendTests passed\n";
    return 0;
}
