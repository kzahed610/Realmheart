#pragma once

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include <memory>
#include <functional>
#include <optional>
#include <array>
#include <filesystem>
#include <string>

#include "relictombs/ManaCoresLayout.hpp"
#include "relictombs/WallpaperLibrary.hpp"
#include "relictombs/RadialBloomShader.hpp"

namespace realmheart::relictombs {

using DismissCallback = std::function<void()>;

class ManaCoresSelector {
public:
    ManaCoresSelector();
    ~ManaCoresSelector();

    [[nodiscard]] bool is_visible() const noexcept { return visible_; }

    void present(GtkApplication* app);
    void dismiss();

    // Set callback invoked when selector is dismissed (Esc or apply complete)
    void set_dismiss_callback(DismissCallback cb) { dismiss_callback_ = std::move(cb); }
    void set_apply_callback(std::function<void(const std::string&)> cb) { apply_callback_ = std::move(cb); }

    // Wallpaper management
    void set_current_wallpaper(GdkPixbuf* pixbuf);
    void set_next_wallpapers(std::array<GdkPixbuf*, 3> pixbufs);
    void load_wallpapers_from_library(const std::filesystem::path& current_path);
    void cycle_wallpaper(int direction);  // -1 = left/prev, +1 = right/next

    // Input handling
    [[nodiscard]] bool handle_key(guint keyval);

    // Public API for shell to trigger apply
    void request_apply();
    void force_apply(const std::string& wallpaper_path);

private:
    // State machine
    enum class State { Hidden, Assembling, Idle, Applying, Dismissing };
    State state_ = State::Hidden;

    // Layout (computed at present time based on monitor)
    ManaCoresLayout layout_;

    // Window and layer surface
    GtkWindow* window_ = nullptr;
    GtkWidget* canvas_ = nullptr;
    bool visible_ = false;

    // Wallpaper pixbufs (non-owning, owned by WallpaperLibrary)
    GdkPixbuf* current_wallpaper_ = nullptr;
    std::array<GdkPixbuf*, 3> next_wallpapers_ = {nullptr, nullptr, nullptr};

    // Animation timing
    guint64 animation_start_micros_ = 0;
    guint64 idle_start_micros_ = 0;
    
    // Current wallpaper index in the cycle
    int current_wallpaper_index_ = 0;

    // Assembly sub-phase
    enum class AssemblePhase { CoreIn, FanIn, Detach };
    AssemblePhase assemble_phase_ = AssemblePhase::CoreIn;

    // Radial animation progress (0..1 each)
    std::array<double, 3> radial_progress_ = {0.0, 0.0, 0.0};
    std::array<double, 3> radial_target_x_ = {0.0, 0.0, 0.0};
    std::array<double, 3> radial_target_y_ = {0.0, 0.0, 0.0};

    // Core radius during animation
    double core_current_radius_ = 0.0;

    // Idle floating parameters (seeded per radial)
    struct IdleFloatParams {
        double period_x = 0.0;
        double period_y = 0.0;
        double phase_x = 0.0;
        double phase_y = 0.0;
        double amplitude_x = 0.0;
        double amplitude_y = 0.0;
    };
    std::array<IdleFloatParams, 3> idle_float_params_;

    // Hovered radial index (-1 = none, 0/1/2 = silver/yellow/orange)
    int hovered_radial_ = -1;
    
    // Wallpaper file paths for cycling
    std::array<std::string, 3> next_wallpaper_paths_ = {};

    // Apply animation
    guint64 apply_start_micros_ = 0;
    double apply_progress_ = 0.0;  // 0..1
    std::unique_ptr<RadialBloomShader> bloom_shader_;

    // Frame clock callback for animations
    guint tick_callback_id_ = 0;
    guint transparency_retry_id_ = 0;
    int transparency_retry_count_ = 0;

    void schedule_transparency_retry();
    static gboolean transparency_retry_callback(GtkWidget* widget, GdkFrameClock* frame_clock, gpointer user_data);

    // Drawing
    void draw_core(cairo_t* cr, double alpha);
    void draw_radials(cairo_t* cr, double alpha);
    void draw_core_animated(cairo_t* cr, double alpha, double radius, double cx, double cy);
    void draw_radials_animated(cairo_t* cr, double alpha);
    void draw_joining_flash(cairo_t* cr, double cx, double cy, double progress);
    void draw_phase10_edge_glow(cairo_t* cr);
    void draw_phase10_motes(cairo_t* cr);
    void draw(GtkDrawingArea* area, cairo_t* cr, int width, int height);
    static void draw_callback(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer user_data);
    static gboolean tick_callback(GtkWidget* widget, GdkFrameClock* frame_clock, gpointer user_data);
    void update_animations(guint64 now_micros);
    void queue_redraw();
    void start_idle_animation();
    void begin_apply();
    void begin_dismiss();
    void setup_window(GtkApplication* app);
    
    // Callbacks
    DismissCallback dismiss_callback_;
    std::function<void(const std::string& /*path*/)> apply_callback_;
};

} // namespace realmheart::relictombs