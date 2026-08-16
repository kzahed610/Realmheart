#pragma once

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include <memory>
#include <functional>
#include <optional>
#include <array>
#include <vector>
#include <filesystem>
#include <string>

#include "relictombs/ManaCoresLayout.hpp"
#include "relictombs/WallpaperLibrary.hpp"

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

    // Assembly sub-phases
    enum class AssemblePhase { Emerge, Formation, Expansion };
    AssemblePhase assemble_phase_ = AssemblePhase::Emerge;

    // Dismiss sub-phases (reverse of assembly)
    enum class DismissPhase { Contraction, Slide };
    DismissPhase dismiss_phase_ = DismissPhase::Contraction;

    // Layout
    ManaCoresLayout layout_;

    // Window and layer surface
    GtkWindow* window_ = nullptr;
    GtkWidget* canvas_ = nullptr;
    bool visible_ = false;

    // Wallpaper library & pixbufs
    std::vector<std::filesystem::path> all_wallpaper_paths_;
    int current_wallpaper_index_ = 0;
    GdkPixbuf* current_core_pixbuf_ = nullptr;
    std::array<GdkPixbuf*, 3> slice_pixbufs_ = {nullptr, nullptr, nullptr};
    GdkPixbuf* old_core_pixbuf_ = nullptr;
    std::array<GdkPixbuf*, 3> old_slice_pixbufs_ = {nullptr, nullptr, nullptr};
    GdkPixbuf* apply_fullscreen_pixbuf_ = nullptr;

    // Animation timing
    guint64 animation_start_micros_ = 0;
    guint64 idle_start_micros_ = 0;
    guint64 apply_start_micros_ = 0;
    guint64 dismiss_start_micros_ = 0;

    // Current animated coordinates & radii
    double current_cx_ = 0.0;
    double current_cy_ = 0.0;
    double current_core_radius_ = 0.0;
    double current_slice_r_in_ = 0.0;
    double current_slice_r_out_ = 0.0;
    std::array<RadialSliceGeometry, 3> current_slices_;

    double current_alpha_ = 1.0;
    double current_wallpaper_alpha_ = 0.0;
    double mana_fill_alpha_ = 1.0;      // Opacity of the mana gradient fill inside slices
    double apply_mask_radius_ = 0.0;

    // Navigation crossfade
    guint64 nav_transition_start_micros_ = 0;
    bool nav_transitioning_ = false;
    double nav_progress_ = 1.0;
    int nav_direction_ = 1;  // +1 = right/next, -1 = left/prev

    // Hovered radial slice index (-1 = none, 0 = silver, 1 = yellow, 2 = orange)
    int hovered_radial_ = -1;
    bool apply_callback_fired_ = false;

    // Frame clock and transparency handling
    guint tick_callback_id_ = 0;
    guint transparency_retry_id_ = 0;
    int transparency_retry_count_ = 0;

    void schedule_transparency_retry();
    static gboolean transparency_retry_callback(GtkWidget* widget, GdkFrameClock* frame_clock, gpointer user_data);

    // Drawing
    void draw(GtkDrawingArea* area, cairo_t* cr, int width, int height);
    static void draw_callback(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer user_data);
    static gboolean tick_callback(GtkWidget* widget, GdkFrameClock* frame_clock, gpointer user_data);

    void draw_core(cairo_t* cr, double cx, double cy, double radius, double alpha, double wallpaper_alpha);
    void draw_radial_slices(cairo_t* cr, double cx, double cy, double r_in, double r_out, double alpha, double wallpaper_alpha);
    void draw_cardinal_stars(cairo_t* cr, double cx, double cy, double radius, double alpha);
    void draw_reverse_bloom(cairo_t* cr, double cx, double cy, double mask_radius);
    void draw_ambient_glow(cairo_t* cr, double cx, double cy, double radius, double alpha);
    void draw_backdrop_dim(cairo_t* cr, double alpha);
    static void draw_pixbuf_cover(cairo_t* cr, GdkPixbuf* pixbuf, double x, double y, double width, double height, double alpha);

    void update_animations(guint64 now_micros);
    void queue_redraw();
    void start_idle_animation();
    void begin_apply();
    void begin_dismiss();
    void setup_window(GtkApplication* app);

    void reload_pixbufs();
    void clear_pixbufs();
    void clear_old_pixbufs();

    // Callbacks
    DismissCallback dismiss_callback_;
    std::function<void(const std::string& /*path*/)> apply_callback_;
};

} // namespace realmheart::relictombs