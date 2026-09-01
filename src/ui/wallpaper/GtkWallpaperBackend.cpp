#include "ui/wallpaper/GtkWallpaperBackend.hpp"

#include <gdk/gdk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace realmheart::ui::wallpaper {

namespace {

void set_error(std::string* destination, const std::string& message) {
    if (destination != nullptr) *destination = message;
}

} // namespace

GtkWallpaperBackend::DecodedWallpaper::~DecodedWallpaper() {
    if (bytes != nullptr) g_bytes_unref(bytes);
}

GtkWallpaperBackend::DecodedWallpaper::DecodedWallpaper(
    DecodedWallpaper&& other
) noexcept
    : bytes(std::exchange(other.bytes, nullptr)),
      width(std::exchange(other.width, 0)),
      height(std::exchange(other.height, 0)),
      channels(std::exchange(other.channels, 0)),
      stride(std::exchange(other.stride, 0)) {}

GtkWallpaperBackend::DecodedWallpaper&
GtkWallpaperBackend::DecodedWallpaper::operator=(DecodedWallpaper&& other) noexcept {
    if (this == &other) return *this;
    if (bytes != nullptr) g_bytes_unref(bytes);
    bytes = std::exchange(other.bytes, nullptr);
    width = std::exchange(other.width, 0);
    height = std::exchange(other.height, 0);
    channels = std::exchange(other.channels, 0);
    stride = std::exchange(other.stride, 0);
    return *this;
}

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

    prepared_wallpaper_.reset();
    prepared_target_.reset();
    auto decoded = decode_wallpaper(path, error_message);
    return decoded && apply_decoded_wallpaper(std::move(*decoded), error_message);
}

bool GtkWallpaperBackend::prepare_wallpaper(
    const std::filesystem::path& path,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_ && !initialize(error_message)) return false;

    prepared_wallpaper_.reset();
    prepared_target_.reset();
    auto decoded = decode_wallpaper(path, error_message);
    if (!decoded) return false;
    prepared_wallpaper_ = std::move(*decoded);
    return true;
}

bool GtkWallpaperBackend::prepare_wallpaper_for_output(
    const std::filesystem::path& path,
    const WallpaperOutputTarget& target,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!target.valid()) {
        set_error(error_message, "wallpaper output target is invalid");
        return false;
    }
    if (!initialized_ && !initialize(error_message)) return false;

    prepared_wallpaper_.reset();
    prepared_target_.reset();
    auto decoded = decode_wallpaper(path, error_message);
    if (!decoded) return false;
    prepared_wallpaper_ = std::move(*decoded);
    prepared_target_ = target;
    return true;
}

bool GtkWallpaperBackend::commit_prepared_wallpaper(
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!prepared_wallpaper_) {
        set_error(error_message, "GTK wallpaper backend has no prepared wallpaper");
        return false;
    }

    auto prepared = std::move(*prepared_wallpaper_);
    prepared_wallpaper_.reset();
    const auto target = std::exchange(prepared_target_, std::nullopt);
    if (target) {
        return apply_decoded_wallpaper_to_output(
            std::move(prepared), *target, error_message
        );
    }
    return apply_decoded_wallpaper(std::move(prepared), error_message);
}

void GtkWallpaperBackend::discard_prepared_wallpaper() noexcept {
    prepared_wallpaper_.reset();
    prepared_target_.reset();
}

std::optional<GtkWallpaperBackend::DecodedWallpaper>
GtkWallpaperBackend::decode_wallpaper(
    const std::filesystem::path& path,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    GError* error = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file(path.c_str(), &error);
    if (pixbuf == nullptr) {
        set_error(
            error_message,
            error != nullptr ? error->message : "unable to decode wallpaper"
        );
        g_clear_error(&error);
        return std::nullopt;
    }
    g_clear_error(&error);

    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const int source_stride = gdk_pixbuf_get_rowstride(pixbuf);
    const auto* source_pixels = gdk_pixbuf_read_pixels(pixbuf);
    if (width <= 0 || height <= 0 || source_pixels == nullptr ||
        (channels != 3 && channels != 4) ||
        gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 ||
        gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB) {
        g_object_unref(pixbuf);
        set_error(error_message, "decoded wallpaper has an unsupported pixel format");
        return std::nullopt;
    }

    const std::size_t stride = static_cast<std::size_t>(width) *
        static_cast<std::size_t>(channels);
    const std::size_t size = stride * static_cast<std::size_t>(height);
    auto* pixels = static_cast<guint8*>(g_malloc(size));
    for (int row = 0; row < height; ++row) {
        std::memcpy(
            pixels + static_cast<std::size_t>(row) * stride,
            source_pixels + static_cast<std::size_t>(row) *
                static_cast<std::size_t>(source_stride),
            stride
        );
    }
    g_object_unref(pixbuf);

    DecodedWallpaper decoded;
    decoded.bytes = g_bytes_new_take(pixels, size);
    decoded.width = width;
    decoded.height = height;
    decoded.channels = channels;
    decoded.stride = stride;
    return decoded;
}

bool GtkWallpaperBackend::apply_decoded_wallpaper(
    DecodedWallpaper&& decoded,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_ && !initialize(error_message)) return false;
    if (decoded.bytes == nullptr || decoded.width <= 0 || decoded.height <= 0 ||
        (decoded.channels != 3 && decoded.channels != 4) || decoded.stride == 0) {
        set_error(error_message, "decoded wallpaper payload is invalid");
        return false;
    }

    const GdkMemoryFormat format = decoded.channels == 4
        ? GDK_MEMORY_R8G8B8A8
        : GDK_MEMORY_R8G8B8;
    GdkTexture* candidate = gdk_memory_texture_new(
        decoded.width,
        decoded.height,
        format,
        decoded.bytes,
        decoded.stride
    );
    if (candidate == nullptr) {
        set_error(error_message, "unable to create wallpaper texture");
        return false;
    }

    GdkTexture* previous = texture_;
    texture_ = candidate;
    clear_output_textures();
    apply_texture(texture_);
    if (previous != nullptr) g_object_unref(previous);
    return true;
}

bool GtkWallpaperBackend::apply_decoded_wallpaper_to_output(
    DecodedWallpaper&& decoded,
    const WallpaperOutputTarget& target,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialized_ && !initialize(error_message)) return false;
    if (!target.valid()) {
        set_error(error_message, "wallpaper output target is invalid");
        return false;
    }
    if (decoded.bytes == nullptr || decoded.width <= 0 || decoded.height <= 0 ||
        (decoded.channels != 3 && decoded.channels != 4) || decoded.stride == 0) {
        set_error(error_message, "decoded wallpaper payload is invalid");
        return false;
    }

    const std::string key = target_key(target);
    const auto found = std::find(surface_keys_.begin(), surface_keys_.end(), key);
    if (found == surface_keys_.end()) {
        set_error(error_message, "requested wallpaper output is unavailable: " + key);
        return false;
    }

    const GdkMemoryFormat format = decoded.channels == 4
        ? GDK_MEMORY_R8G8B8A8
        : GDK_MEMORY_R8G8B8;
    GdkTexture* candidate = gdk_memory_texture_new(
        decoded.width,
        decoded.height,
        format,
        decoded.bytes,
        decoded.stride
    );
    if (candidate == nullptr) {
        set_error(error_message, "unable to create wallpaper texture");
        return false;
    }

    const std::size_t surface_index = static_cast<std::size_t>(
        std::distance(surface_keys_.begin(), found)
    );
    if (surface_index >= surfaces_.size()) {
        g_object_unref(candidate);
        set_error(error_message, "requested wallpaper surface is unavailable");
        return false;
    }

    auto existing = output_textures_.find(key);
    GdkTexture* previous = existing != output_textures_.end()
        ? existing->second
        : nullptr;
    output_textures_[key] = candidate;
    surfaces_[surface_index]->set_paintable(GDK_PAINTABLE(candidate));
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
    std::vector<std::string> replacement_keys;
    const guint count = g_list_model_get_n_items(monitors_);
    replacements.reserve(count);
    replacement_keys.reserve(count);

    for (guint index = 0; index < count; ++index) {
        GdkMonitor* monitor = GDK_MONITOR(g_list_model_get_item(monitors_, index));
        if (monitor == nullptr) continue;

        const std::string key = output_key(static_cast<int>(index), monitor);
        const auto override = output_textures_.find(key);
        GdkTexture* effective = override != output_textures_.end()
            ? override->second
            : texture_;

        try {
            replacements.push_back(std::make_unique<WallpaperSurface>(
                application_,
                monitor,
                static_cast<int>(index),
                effective != nullptr ? GDK_PAINTABLE(effective) : nullptr
            ));
            replacement_keys.push_back(key);
        } catch (...) {
            g_object_unref(monitor);
            throw;
        }
        g_object_unref(monitor);
    }

    surfaces_.swap(replacements);
    surface_keys_.swap(replacement_keys);
}

void GtkWallpaperBackend::apply_texture(GdkTexture* texture) {
    for (const auto& surface : surfaces_) {
        surface->set_paintable(
            texture != nullptr ? GDK_PAINTABLE(texture) : nullptr
        );
    }
}

std::string GtkWallpaperBackend::output_key(
    int monitor_index,
    GdkMonitor* monitor
) const {
    if (monitor != nullptr) {
        if (const char* connector = gdk_monitor_get_connector(monitor);
            connector != nullptr && *connector != '\0') {
            return connector;
        }
    }
    return "#" + std::to_string(std::max(monitor_index, 0));
}

std::string GtkWallpaperBackend::target_key(
    const WallpaperOutputTarget& target
) const {
    if (!target.connector.empty()) return target.connector;
    return "#" + std::to_string(std::max(target.monitor_index, 0));
}

void GtkWallpaperBackend::clear_output_textures() noexcept {
    for (auto& [_, texture] : output_textures_) {
        if (texture != nullptr) g_object_unref(texture);
    }
    output_textures_.clear();
}

void GtkWallpaperBackend::reset() noexcept {
    if (monitors_ != nullptr && monitor_signal_id_ != 0) {
        g_signal_handler_disconnect(monitors_, monitor_signal_id_);
        monitor_signal_id_ = 0;
    }

    surfaces_.clear();
    surface_keys_.clear();
    prepared_wallpaper_.reset();
    prepared_target_.reset();
    clear_output_textures();
    if (texture_ != nullptr) {
        g_object_unref(texture_);
        texture_ = nullptr;
    }

    monitors_ = nullptr;
    display_ = nullptr;
    initialized_ = false;
}

} // namespace realmheart::ui::wallpaper
