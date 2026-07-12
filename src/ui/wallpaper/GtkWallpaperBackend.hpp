#pragma once

#include "ui/wallpaper/WallpaperBackend.hpp"
#include "ui/wallpaper/WallpaperSurface.hpp"

#include <gtk/gtk.h>

#include <memory>
#include <vector>

namespace realmheart::ui::wallpaper {

class GtkWallpaperBackend final : public WallpaperBackend {
public:
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
