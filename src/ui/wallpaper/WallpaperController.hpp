#pragma once

#include "ui/wallpaper/WallpaperBackend.hpp"

#include <gtk/gtk.h>

#include <filesystem>
#include <memory>
#include <string>

namespace realmheart::ui::wallpaper {

class WallpaperController {
public:
    WallpaperController(
        GtkApplication* application,
        WallpaperBackendType requested_backend
    );
    ~WallpaperController() = default;

    WallpaperController(const WallpaperController&) = delete;
    WallpaperController& operator=(const WallpaperController&) = delete;
    WallpaperController(WallpaperController&&) = delete;
    WallpaperController& operator=(WallpaperController&&) = delete;

    [[nodiscard]] bool initialize(std::string* error_message = nullptr);
    [[nodiscard]] bool set_wallpaper(
        const std::filesystem::path& path,
        std::string* error_message = nullptr
    );
    [[nodiscard]] bool switch_backend(
        WallpaperBackendType backend,
        std::string* error_message = nullptr
    );

    [[nodiscard]] WallpaperBackendType active_backend() const noexcept;

private:
    [[nodiscard]] std::unique_ptr<WallpaperBackend> create_backend(
        WallpaperBackendType type
    ) const;
    [[nodiscard]] bool activate_backend(
        WallpaperBackendType type,
        std::string* error_message
    );

    GtkApplication* application_ = nullptr;
    WallpaperBackendType requested_backend_ = WallpaperBackendType::Gtk;
    std::unique_ptr<WallpaperBackend> backend_;
    std::filesystem::path current_wallpaper_;
};

} // namespace realmheart::ui::wallpaper
