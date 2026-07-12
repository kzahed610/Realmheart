#include "ui/wallpaper/WallpaperManager.hpp"
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <filesystem>

namespace realmheart::ui::wallpaper {

WallpaperManager::WallpaperManager(GtkApplication* application)
    : application_(application), display_(gdk_display_get_default()) {
    if (application_ == nullptr) throw std::invalid_argument("wallpaper manager requires a GTK application");
    if (display_ == nullptr) throw std::runtime_error("wallpaper manager requires an active GDK display");

    monitors_ = gdk_display_get_monitors(display_);
    if (monitors_ == nullptr) throw std::runtime_error("GDK display has no monitor model");

    rebuild_surfaces();
    monitor_signal_id_ = g_signal_connect(
        monitors_,
        "items-changed",
        G_CALLBACK(&WallpaperManager::on_monitors_changed),
        this
    );
}

WallpaperManager::~WallpaperManager() {
    if (monitors_ != nullptr && monitor_signal_id_ != 0) {
        g_signal_handler_disconnect(monitors_, monitor_signal_id_);
        monitor_signal_id_ = 0;
    }

    surfaces_.clear();
    if (texture_ != nullptr) {
        g_object_unref(texture_);
        texture_ = nullptr;
    }
}

bool WallpaperManager::set_wallpaper(
    const std::filesystem::path& path,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();

    GError* error = nullptr;
    // 1. Load as GdkPixbuf to allow scaling
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &error);
    if (pixbuf == nullptr) {
        if (error_message != nullptr) {
            *error_message = error != nullptr ? error->message : "unable to decode wallpaper";
        }
        if (error != nullptr) g_error_free(error);
        return false;
    }
    if (error != nullptr) g_error_free(error);

    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);

    GdkPixbuf* final_pixbuf = pixbuf;

    // 2. Downscale to 1080p (1920x1080) if image is larger
    if (width > 1920 || height > 1080) {
        double scale_x = 1920.0 / width;
        double scale_y = 1080.0 / height;
        double scale = std::min(scale_x, scale_y);

        int new_width = static_cast<int>(width * scale);
        int new_height = static_cast<int>(height * scale);

        final_pixbuf = gdk_pixbuf_scale_simple(pixbuf, new_width, new_height, GDK_INTERP_BILINEAR);
        g_object_unref(pixbuf);
    }

    // 3. Convert GdkPixbuf to GdkTexture for GTK4 rendering
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    GdkTexture* candidate = gdk_texture_new_for_pixbuf(final_pixbuf);
    #pragma GCC diagnostic pop
    
    // The texture retains the backing pixbuf.
    g_object_unref(final_pixbuf);

    if (candidate == nullptr) {
        if (error_message != nullptr) *error_message = "failed to convert pixbuf to texture";
        return false;
    }

    GdkTexture* previous = texture_;
    texture_ = candidate;
    apply_texture(texture_);
    if (previous != nullptr) g_object_unref(previous);

    return true;
}

void WallpaperManager::on_monitors_changed(
    GListModel* /*monitors*/,
    guint /*position*/,
    guint /*removed*/,
    guint /*added*/,
    gpointer user_data
) {
    auto* self = static_cast<WallpaperManager*>(user_data);
    try {
        self->rebuild_surfaces();
    } catch (const std::exception& error) {
        std::cerr << "Unable to rebuild wallpaper surfaces: " << error.what() << '\n';
    }
}

void WallpaperManager::rebuild_surfaces() {
    std::vector<std::unique_ptr<WallpaperSurface>> replacements;
    const guint count = g_list_model_get_n_items(monitors_);
    replacements.reserve(count);

    for (guint index = 0; index < count; ++index) {
        GdkMonitor* monitor = GDK_MONITOR(g_list_model_get_item(monitors_, index));
        if (monitor == nullptr) continue;

        try {
            replacements.push_back(std::make_unique<WallpaperSurface>(
                application_, monitor, texture_ != nullptr ? GDK_PAINTABLE(texture_) : nullptr
            ));
        } catch (...) {
            g_object_unref(monitor);
            throw;
        }
        g_object_unref(monitor);
    }

    surfaces_.swap(replacements);
}

void WallpaperManager::apply_texture(GdkTexture* texture) {
    for (const auto& surface : surfaces_) {
        surface->set_paintable(texture != nullptr ? GDK_PAINTABLE(texture) : nullptr);
    }
}

} // namespace realmheart::ui::wallpaper
