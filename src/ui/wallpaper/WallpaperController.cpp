#include "ui/wallpaper/WallpaperController.hpp"

#include "ui/wallpaper/GtkWallpaperBackend.hpp"
#include "ui/wallpaper/NativeWallpaperBackend.hpp"

#include <iostream>
#include <utility>

namespace realmheart::ui::wallpaper {

namespace {

void set_error(std::string* destination, const std::string& message) {
    if (destination != nullptr) *destination = message;
}

} // namespace

WallpaperController::WallpaperController(
    GtkApplication* application,
    WallpaperBackendType requested_backend
) : application_(application), requested_backend_(requested_backend) {}

bool WallpaperController::initialize(std::string* error_message) {
    if (error_message != nullptr) error_message->clear();
    if (backend_ != nullptr) return true;

    std::string requested_error;
    if (activate_backend(requested_backend_, &requested_error)) return true;

    if (requested_backend_ == WallpaperBackendType::Native) {
        std::cerr << "Native wallpaper backend unavailable: "
                  << requested_error << "; falling back to GTK\n";
        if (activate_backend(WallpaperBackendType::Gtk, error_message)) return true;
    }

    set_error(error_message, requested_error);
    return false;
}

bool WallpaperController::set_wallpaper(
    const std::filesystem::path& path,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (!initialize(error_message)) return false;

    std::string backend_error;
    if (backend_->set_wallpaper(path, &backend_error)) {
        current_wallpaper_ = path;
        return true;
    }

    if (backend_->type() == WallpaperBackendType::Native) {
        std::cerr << "Native wallpaper backend failed: " << backend_error
                  << "; switching to GTK\n";
        if (activate_backend(WallpaperBackendType::Gtk, error_message) &&
            backend_->set_wallpaper(path, error_message)) {
            current_wallpaper_ = path;
            return true;
        }
        return false;
    }

    set_error(error_message, backend_error);
    return false;
}

bool WallpaperController::switch_backend(
    WallpaperBackendType backend,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (backend_ != nullptr && backend_->type() == backend) return true;

    auto candidate = create_backend(backend);
    std::string candidate_error;
    if (!candidate->initialize(&candidate_error)) {
        set_error(error_message, candidate_error);
        return false;
    }

    if (!current_wallpaper_.empty() &&
        !candidate->set_wallpaper(current_wallpaper_, &candidate_error)) {
        set_error(error_message, candidate_error);
        return false;
    }

    backend_ = std::move(candidate);
    requested_backend_ = backend;
    return true;
}

WallpaperBackendType WallpaperController::active_backend() const noexcept {
    return backend_ != nullptr ? backend_->type() : requested_backend_;
}

std::unique_ptr<WallpaperBackend> WallpaperController::create_backend(
    WallpaperBackendType type
) const {
    switch (type) {
    case WallpaperBackendType::Gtk:
        return std::make_unique<GtkWallpaperBackend>(application_);
    case WallpaperBackendType::Native:
        return std::make_unique<NativeWallpaperBackend>();
    }
    return std::make_unique<GtkWallpaperBackend>(application_);
}

bool WallpaperController::activate_backend(
    WallpaperBackendType type,
    std::string* error_message
) {
    auto candidate = create_backend(type);
    if (!candidate->initialize(error_message)) return false;
    backend_ = std::move(candidate);
    return true;
}

} // namespace realmheart::ui::wallpaper
