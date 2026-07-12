#pragma once

#include "ui/wallpaper/WallpaperSurface.hpp"

#include <gtk/gtk.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace realmheart::ui::wallpaper {

class WallpaperManager {
public:
    explicit WallpaperManager(GtkApplication* application);
    ~WallpaperManager();

    WallpaperManager(const WallpaperManager&) = delete;
    WallpaperManager& operator=(const WallpaperManager&) = delete;
    WallpaperManager(WallpaperManager&&) = delete;
    WallpaperManager& operator=(WallpaperManager&&) = delete;

    [[nodiscard]] bool set_wallpaper(
        const std::filesystem::path& path,
        std::string* error_message = nullptr
    );

private:
    static void on_monitors_changed(
        GListModel* monitors,
        guint position,
        guint removed,
        guint added,
        gpointer user_data
    );

    void rebuild_surfaces();
    void apply_texture(GdkTexture* texture);

    GtkApplication* application_ = nullptr;
    GdkDisplay* display_ = nullptr;
    GListModel* monitors_ = nullptr;
    gulong monitor_signal_id_ = 0;
    GdkTexture* texture_ = nullptr;
    std::vector<std::unique_ptr<WallpaperSurface>> surfaces_;
};

} // namespace realmheart::ui::wallpaper
