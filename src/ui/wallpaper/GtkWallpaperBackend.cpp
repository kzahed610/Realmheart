#include "ui/wallpaper/GtkWallpaperBackend.hpp"

#include <gdk/gdk.h>

#include <exception>
#include <iostream>
#include <stdexcept>

namespace realmheart::ui::wallpaper {

namespace {

void set_error(std::string* destination, const std::string& message) {
    if (destination != nullptr) *destination = message;
}

} // namespace

GtkWallpaperBackend::GtkWallpaperBackend(GtkApplication* application)
    : application_(application) {}

GtkWallpaperBackend::~GtkWallpaperBackend() {
    reset();
}

bool GtkWallpaperBackend::initialize(std::string* error_message) {
    if (error_message != nullptr) error_message->clear();
    if (initialized_) return true;

    if (application_ == nullptr) {
        set_error(error_message, "GTK wallpaper backend requires an application");
        return false;
    }

    display_ = gdk_display_get_default();
    if (display_ == nullptr) {
        set_error(error_message, "GTK wallpaper backend requires an active display");
        return false;
    }

    monitors_ = gdk_display_get_monitors(display_);
    if (monitors_ == nullptr) {
        set_error(error_message, "GTK display did not expose a monitor model");
        return false;
    }

    try {
        rebuild_surfaces();
    } catch (const std::exception& error) {
        reset();
        set_error(error_message, error.what());
        return false;
    }

    monitor_signal_id_ = g_signal_connect(
        monitors_,
        "items-changed",
        G_CALLBACK(&GtkWallpaperBackend::on_monitors_changed),
        this
    );
    initialized_ = true;
    return true;
}

bool GtkWallpaperBackend::set_wallpaper(
    const std::filesystem::path& path,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_ && !initialize(error_message)) return false;

    GError* error = nullptr;
    GdkTexture* candidate = gdk_texture_new_from_filename(path.c_str(), &error);
    if (candidate == nullptr) {
        set_error(
            error_message,
            error != nullptr ? error->message : "unable to decode wallpaper"
        );
        if (error != nullptr) g_error_free(error);
        return false;
    }
    if (error != nullptr) g_error_free(error);

    GdkTexture* previous = texture_;
    texture_ = candidate;
    apply_texture(texture_);
    if (previous != nullptr) g_object_unref(previous);
    return true;
}

void GtkWallpaperBackend::on_monitors_changed(
    GListModel* /*monitors*/,
    guint /*position*/,
    guint /*removed*/,
    guint /*added*/,
    gpointer user_data
) {
    auto* self = static_cast<GtkWallpaperBackend*>(user_data);
    try {
        self->rebuild_surfaces();
    } catch (const std::exception& error) {
        std::cerr << "Unable to rebuild GTK wallpaper surfaces: "
                  << error.what() << '\n';
    }
}

void GtkWallpaperBackend::rebuild_surfaces() {
    std::vector<std::unique_ptr<WallpaperSurface>> replacements;
    const guint count = g_list_model_get_n_items(monitors_);
    replacements.reserve(count);

    for (guint index = 0; index < count; ++index) {
        GdkMonitor* monitor = GDK_MONITOR(g_list_model_get_item(monitors_, index));
        if (monitor == nullptr) continue;

        try {
            replacements.push_back(std::make_unique<WallpaperSurface>(
                application_,
                monitor,
                texture_ != nullptr ? GDK_PAINTABLE(texture_) : nullptr
            ));
        } catch (...) {
            g_object_unref(monitor);
            throw;
        }
        g_object_unref(monitor);
    }

    surfaces_.swap(replacements);
}

void GtkWallpaperBackend::apply_texture(GdkTexture* texture) {
    for (const auto& surface : surfaces_) {
        surface->set_paintable(
            texture != nullptr ? GDK_PAINTABLE(texture) : nullptr
        );
    }
}

void GtkWallpaperBackend::reset() noexcept {
    if (monitors_ != nullptr && monitor_signal_id_ != 0) {
        g_signal_handler_disconnect(monitors_, monitor_signal_id_);
        monitor_signal_id_ = 0;
    }

    surfaces_.clear();
    if (texture_ != nullptr) {
        g_object_unref(texture_);
        texture_ = nullptr;
    }

    monitors_ = nullptr;
    display_ = nullptr;
    initialized_ = false;
}

} // namespace realmheart::ui::wallpaper
