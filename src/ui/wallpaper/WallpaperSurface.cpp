#include "ui/wallpaper/WallpaperSurface.hpp"
#include "ui/LayerSurface.hpp"
#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <stdexcept>

namespace realmheart::ui::wallpaper {

WallpaperSurface::WallpaperSurface(
    GtkApplication* application,
    GdkMonitor* monitor,
    GdkPaintable* initial_paintable
) {
    if (application == nullptr) throw std::invalid_argument("wallpaper surface requires a GTK application");
    if (monitor == nullptr) throw std::invalid_argument("wallpaper surface requires a monitor");

    window_ = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(window_, "Realmheart Wallpaper");
    gtk_window_set_decorated(window_, FALSE);
    gtk_window_set_resizable(window_, TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(window_), FALSE);

    picture_ = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(picture_, GTK_CONTENT_FIT_COVER);
    gtk_picture_set_can_shrink(picture_, TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(picture_), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(picture_), TRUE);
    gtk_window_set_child(window_, GTK_WIDGET(picture_));

    apply_layer_surface(window_, make_wallpaper_surface_spec());
    gtk_layer_set_monitor(window_, monitor);

    if (initial_paintable != nullptr) {
        gtk_picture_set_paintable(picture_, initial_paintable);
    }

    gtk_window_present(window_);
}

WallpaperSurface::~WallpaperSurface() {
    if (window_ != nullptr) {
        gtk_window_destroy(window_);
        window_ = nullptr;
        picture_ = nullptr;
    }
}

void WallpaperSurface::set_paintable(GdkPaintable* paintable) {
    if (picture_ == nullptr) return;
    gtk_picture_set_paintable(picture_, nullptr);
    gtk_picture_set_paintable(picture_, paintable);
}

} // namespace realmheart::ui::wallpaper
