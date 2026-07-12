#include "ui/wallpaper/WallpaperBackend.hpp"

#include <cstdlib>

namespace realmheart::ui::wallpaper {

std::optional<WallpaperBackendType> parse_wallpaper_backend_type(
    std::string_view value
) {
    if (value == "gtk" || value == "gtk4") return WallpaperBackendType::Gtk;
    if (value == "native" || value == "wayland" || value == "gles") {
        return WallpaperBackendType::Native;
    }
    return std::nullopt;
}

std::string_view wallpaper_backend_type_name(WallpaperBackendType type) noexcept {
    switch (type) {
    case WallpaperBackendType::Gtk:
        return "gtk";
    case WallpaperBackendType::Native:
        return "native";
    }
    return "gtk";
}

WallpaperBackendType wallpaper_backend_from_environment() noexcept {
    const char* value = std::getenv("REALMHEART_WALLPAPER_BACKEND");
    if (value == nullptr) return WallpaperBackendType::Gtk;

    const auto parsed = parse_wallpaper_backend_type(value);
    return parsed.value_or(WallpaperBackendType::Gtk);
}

} // namespace realmheart::ui::wallpaper
