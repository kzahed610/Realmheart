#pragma once
#include <gtk/gtk.h>

namespace realmheart::ui::wallpaper {

class WallpaperSurface {
public:
    WallpaperSurface(GtkApplication* application, GdkMonitor* monitor, GdkPaintable* initial_paintable = nullptr);
    ~WallpaperSurface();

    WallpaperSurface(const WallpaperSurface&) = delete;
    WallpaperSurface& operator=(const WallpaperSurface&) = delete;
    WallpaperSurface(WallpaperSurface&&) = delete;
    WallpaperSurface& operator=(WallpaperSurface&&) = delete;

    void set_paintable(GdkPaintable* paintable);

private:
    GtkWindow* window_ = nullptr;
    GtkPicture* picture_ = nullptr;
};

} // namespace realmheart::ui::wallpaper
