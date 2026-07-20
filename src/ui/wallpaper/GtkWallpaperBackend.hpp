#pragma once

#include "ui/wallpaper/WallpaperBackend.hpp"
#include "ui/wallpaper/WallpaperSurface.hpp"

#include <gtk/gtk.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace realmheart::ui::wallpaper {

class GtkWallpaperBackend final : public WallpaperBackend {
public:
    struct DecodedWallpaper {
        DecodedWallpaper() = default;
        ~DecodedWallpaper();
        DecodedWallpaper(const DecodedWallpaper&) = delete;
        DecodedWallpaper& operator=(const DecodedWallpaper&) = delete;
        DecodedWallpaper(DecodedWallpaper&& other) noexcept;
        DecodedWallpaper& operator=(DecodedWallpaper&& other) noexcept;

        GBytes* bytes = nullptr;
        int width = 0;
        int height = 0;
        int channels = 0;
        std::size_t stride = 0;
    };

    explicit GtkWallpaperBackend(GtkApplication* application);
    ~GtkWallpaperBackend() override;

    [[nodiscard]] WallpaperBackendType type() const noexcept override {
        return WallpaperBackendType::Gtk;
    }

    [[nodiscard]] bool initialize(std::string* error_message = nullptr) override;
    [[nodiscard]] bool set_wallpaper(
        const std::filesystem::path& path,
        std::string* error_message = nullptr
    ) override;

    [[nodiscard]] static std::optional<DecodedWallpaper> decode_wallpaper(
        const std::filesystem::path& path,
        std::string* error_message = nullptr
    );
    [[nodiscard]] bool apply_decoded_wallpaper(
        DecodedWallpaper&& decoded,
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
    void reset() noexcept;

    GtkApplication* application_ = nullptr;
    GdkDisplay* display_ = nullptr;
    GListModel* monitors_ = nullptr;
    gulong monitor_signal_id_ = 0;
    GdkTexture* texture_ = nullptr;
    std::vector<std::unique_ptr<WallpaperSurface>> surfaces_;
    bool initialized_ = false;
};

} // namespace realmheart::ui::wallpaper
