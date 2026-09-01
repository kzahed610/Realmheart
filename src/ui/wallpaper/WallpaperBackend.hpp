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

struct WallpaperOutputTarget {
    int monitor_index = -1;
    std::string connector;

    [[nodiscard]] bool valid() const noexcept {
        return monitor_index >= 0 || !connector.empty();
    }
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

    // Generic two-phase wallpaper transaction. prepare_wallpaper() performs
    // expensive decode/upload work without changing visible pixels;
    // commit_prepared_wallpaper() makes that prepared image authoritative.
    [[nodiscard]] virtual bool prepare_wallpaper(
        const std::filesystem::path& path,
        std::string* error_message = nullptr
    ) = 0;
    [[nodiscard]] virtual bool prepare_wallpaper_for_output(
        const std::filesystem::path& path,
        const WallpaperOutputTarget& target,
        std::string* error_message = nullptr
    ) = 0;
    [[nodiscard]] virtual bool commit_prepared_wallpaper(
        std::string* error_message = nullptr
    ) = 0;
    virtual void discard_prepared_wallpaper() noexcept = 0;

protected:
    WallpaperBackend() = default;
};

} // namespace realmheart::ui::wallpaper
