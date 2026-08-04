#pragma once

#include "animation/layered/DirectionalStripMesh.hpp"
#include "ui/powermenu/PowerMenuAnimator.hpp"
#include "ui/powermenu/PowerMenuManifest.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <memory>
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
    void set_visibility_callback(std::function<void(double)> callback);
    void present();
    void dismiss(std::function<void()> on_hidden);
    void hide_immediately();
    void set_confirming(bool confirming);

private:
    struct LoadedLayer {
        std::string name;
        GdkTexture* texture = nullptr;
        std::vector<GdkTexture*> flow_poses;
        std::optional<animation::layered::DirectionalStripMesh> mesh;
        PowerMenuAnimationRig animation;
        PowerMenuPlacement placement;
        double flow_minimum = 0.0;
        double flow_maximum = 0.0;
        std::size_t decoded_bytes = 0;
    };

    static void snapshot_callback(
        GtkWidget* widget,
        GtkSnapshot* snapshot,
        gpointer user_data
    );
    static gboolean timer_callback(gpointer user_data);

    bool load();
    void snapshot(GtkWidget* widget, GtkSnapshot* snapshot) const;
    gboolean on_timer();
    void ensure_tick();
    void stop_tick();
    void publish_visibility();
    void release_layers() noexcept;

    GtkWidget* widget_ = nullptr;
    std::optional<PowerMenuManifest> manifest_;
    std::optional<PowerMenuRig> rig_;
    std::unique_ptr<PowerMenuAnimator> animator_;
    std::vector<LoadedLayer> layers_;
    std::string error_message_;
    std::function<void()> on_hidden_;
    std::function<void(double)> visibility_callback_;
    guint tick_callback_id_ = 0;
    gint64 last_frame_time_us_ = 0;
    double idle_accumulator_seconds_ = 0.0;
};

} // namespace realmheart::ui::powermenu
