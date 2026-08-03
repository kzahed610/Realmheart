#pragma once

#include "ui/powermenu/PowerMenuManifest.hpp"

#include <gtk/gtk.h>

#include <optional>
#include <string>
#include <vector>

namespace realmheart::ui::powermenu {

class PowerMenuScene {
public:
    PowerMenuScene();
    ~PowerMenuScene();

    PowerMenuScene(const PowerMenuScene&) = delete;
    PowerMenuScene& operator=(const PowerMenuScene&) = delete;

    [[nodiscard]] GtkWidget* widget() const;
    [[nodiscard]] bool ready() const;
    [[nodiscard]] const std::string& error_message() const;

private:
    struct LoadedLayer {
        std::string name;
        GdkTexture* texture = nullptr;
        PowerMenuPlacement placement;
    };

    static void snapshot_callback(
        GtkWidget* widget,
        GtkSnapshot* snapshot,
        gpointer user_data
    );

    bool load();
    void snapshot(GtkWidget* widget, GtkSnapshot* snapshot) const;
    void release_layers() noexcept;

    GtkWidget* widget_ = nullptr;
    std::optional<PowerMenuManifest> manifest_;
    std::vector<LoadedLayer> layers_;
    std::string error_message_;
};

} // namespace realmheart::ui::powermenu
