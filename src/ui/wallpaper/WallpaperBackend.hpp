#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace realmheart::ui::wallpaper {

enum class WallpaperBackendType {
    Gtk,
    Native,
};

[[nodiscard]] std::optional<WallpaperBackendType> parse_wallpaper_backend_type(
    std::string_view value
);

[[nodiscard]] std::string_view wallpaper_backend_type_name(
    WallpaperBackendType type
) noexcept;

[[nodiscard]] WallpaperBackendType wallpaper_backend_from_environment() noexcept;

class WallpaperBackend {
public:
    virtual ~WallpaperBackend() = default;

    WallpaperBackend(const WallpaperBackend&) = delete;
    WallpaperBackend& operator=(const WallpaperBackend&) = delete;
    WallpaperBackend(WallpaperBackend&&) = delete;
    WallpaperBackend& operator=(WallpaperBackend&&) = delete;

    [[nodiscard]] virtual WallpaperBackendType type() const noexcept = 0;
    [[nodiscard]] virtual bool initialize(std::string* error_message = nullptr) = 0;
    [[nodiscard]] virtual bool set_wallpaper(
        const std::filesystem::path& path,
        std::string* error_message = nullptr
    ) = 0;

protected:
    WallpaperBackend() = default;
};

} // namespace realmheart::ui::wallpaper
