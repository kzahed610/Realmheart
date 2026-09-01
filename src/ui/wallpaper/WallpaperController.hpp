#pragma once

#include "ui/wallpaper/WallpaperBackend.hpp"

#include <gtk/gtk.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace realmheart::ui::wallpaper {

class WallpaperController {
public:
    using SetWallpaperCallback = std::function<void(bool, std::string)>;

    WallpaperController(
        GtkApplication* application,
        WallpaperBackendType requested_backend
    );
    ~WallpaperController();

    WallpaperController(const WallpaperController&) = delete;
    WallpaperController& operator=(const WallpaperController&) = delete;
    WallpaperController(WallpaperController&&) = delete;
    WallpaperController& operator=(WallpaperController&&) = delete;

    [[nodiscard]] bool initialize(std::string* error_message = nullptr);
    [[nodiscard]] bool set_wallpaper(
        const std::filesystem::path& path,
        std::string* error_message = nullptr
    );
    void set_wallpaper_async(
        std::filesystem::path path,
        SetWallpaperCallback callback = {}
    );
    void prepare_wallpaper_async(
        std::filesystem::path path,
        SetWallpaperCallback callback = {}
    );
    void prepare_wallpaper_for_output_async(
        std::filesystem::path path,
        WallpaperOutputTarget target,
        SetWallpaperCallback callback = {}
    );
    void commit_prepared_wallpaper_async(
        SetWallpaperCallback callback = {}
    );
    void discard_prepared_wallpaper() noexcept;
    void switch_backend_async(
        WallpaperBackendType backend,
        SetWallpaperCallback callback = {}
    );
    [[nodiscard]] bool switch_backend(
        WallpaperBackendType backend,
        std::string* error_message = nullptr
    );

    [[nodiscard]] WallpaperBackendType active_backend() const noexcept;

private:
    struct AsyncState {
        std::atomic<bool> alive{true};
        std::atomic<std::uint64_t> generation{0};
        std::atomic<WallpaperController*> owner{nullptr};
    };

    [[nodiscard]] std::shared_ptr<WallpaperBackend> create_backend(
        WallpaperBackendType type
    ) const;
    [[nodiscard]] bool activate_backend(
        WallpaperBackendType type,
        std::string* error_message
    );
    void start_gtk_request(
        std::shared_ptr<WallpaperBackend> backend,
        std::filesystem::path path,
        std::uint64_t generation,
        SetWallpaperCallback callback
    );

    GtkApplication* application_ = nullptr;
    WallpaperBackendType requested_backend_ = WallpaperBackendType::Gtk;
    std::shared_ptr<WallpaperBackend> backend_;
    std::filesystem::path current_wallpaper_;
    std::filesystem::path prepared_wallpaper_;
    std::shared_ptr<AsyncState> async_state_ = std::make_shared<AsyncState>();
};

} // namespace realmheart::ui::wallpaper
