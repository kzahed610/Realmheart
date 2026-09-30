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
#include <atomic>
#include <cstdint>

#include "mana_core/ManaCoresLayout.hpp"
#include "mana_core/WallpaperLibrary.hpp"

namespace realmheart::mana_core {

using DismissCallback = std::function<void()>;
using ApplyCompletion = std::function<void(bool, std::string)>;
using ApplyPreparationCallback = std::function<void(
    const std::string&,
    ApplyCompletion
)>;
using ApplyCallback = std::function<void(const std::string&, ApplyCompletion)>;
using ApplyAbortCallback = std::function<void()>;

class ManaCoresSelector {
public:
    ManaCoresSelector();
    ~ManaCoresSelector();

    [[nodiscard]] bool is_visible() const noexcept { return visible_; }

    void present(GtkApplication* app, int monitor_index = -1);
    void dismiss();

    // Set callback invoked when selector is dismissed (Esc or apply complete)
    void set_dismiss_callback(DismissCallback cb) { dismiss_callback_ = std::move(cb); }
    void set_apply_prepare_callback(ApplyPreparationCallback cb) {
        apply_prepare_callback_ = std::move(cb);
    }
    void set_apply_callback(ApplyCallback cb) { apply_callback_ = std::move(cb); }
    void set_apply_abort_callback(ApplyAbortCallback cb) {
        apply_abort_callback_ = std::move(cb);
    }

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

    // Public API for shell to trigger animated dismiss (keybind toggle)
    void request_dismiss();

private:
    struct AsyncState {
        std::atomic<bool> alive{true};
        std::atomic<ManaCoresSelector*> owner{nullptr};
        std::atomic<std::uint64_t> generation{0};
    };

    struct ApplyOutputGeometry {
        int width = 0;
        int height = 0;
        int scale_factor = 1;

        bool operator==(const ApplyOutputGeometry&) const = default;
    };

    // State machine
    enum class State { Hidden, Assembling, Idle, PreparingApply, Applying, Dismissing };
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
    int monitor_index_ = -1;

    // Wallpaper library & pixbufs
    std::vector<std::filesystem::path> all_wallpaper_paths_;
    std::vector<bool> wallpaper_decode_ready_;
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
    guint64 apply_handoff_start_micros_ = 0;
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
    double apply_fullscreen_alpha_ = 1.0;
    static constexpr guint64 kApplyHandoffDurationMicros = 180'000;

    // Navigation crossfade
    guint64 nav_transition_start_micros_ = 0;
    bool nav_transitioning_ = false;
    double nav_progress_ = 1.0;
    int nav_direction_ = 1;  // +1 = right/next, -1 = left/prev
    bool pending_navigation_ = false;
    int pending_navigation_direction_ = 1;
    bool initial_preview_ready_ = false;

    // Hovered radial slice index (-1 = none, 0 = silver, 1 = yellow, 2 = orange)
    int hovered_radial_ = -1;
    bool apply_callback_fired_ = false;
    bool apply_preview_ready_ = false;
    bool apply_backend_ready_ = false;
    bool apply_animation_finished_ = false;
    bool apply_commit_finished_ = false;
    bool apply_commit_success_ = false;
    bool apply_handoff_zero_frame_queued_ = false;
    bool apply_fullscreen_opaque_ = false;
    ApplyOutputGeometry apply_output_geometry_;
    std::int64_t apply_cover_frame_counter_ = 0;
    std::uint64_t apply_generation_ = 0;
    std::string applying_wallpaper_path_;

    // Atmospheric Mana & Aether Particle System (zero-allocation fixed pool)
    struct ManaParticle {
        double x = 0.0;
        double y = 0.0;
        double vx = 0.0;
        double vy = 0.0;
        double life = 0.0;    // 1.0 -> 0.0
        double decay = 0.02;
        double size = 2.0;
        double r = 1.0;
        double g = 1.0;
        double b = 1.0;
        bool active = false;
    };
    static constexpr size_t kMaxParticles = 36;
    std::array<ManaParticle, kMaxParticles> particles_{};
    guint64 last_tick_micros_ = 0;

    // Shader-based White Core Smoke (GLSL / GtkGLArea)
    GtkWidget* gl_area_ = nullptr;
    unsigned int gl_program_ = 0;
    unsigned int gl_vao_ = 0;
    bool ensure_gl_program();
    void cleanup_gl_resources() noexcept;
    gboolean render_gl(GtkGLArea* area, GdkGLContext* context) noexcept;
    static gboolean gl_render_callback(GtkGLArea* area, GdkGLContext* context, gpointer user_data);
    static void gl_unrealize_callback(GtkWidget* widget, gpointer user_data);

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

    void draw_drop_shadows(cairo_t* cr, double cx, double cy, double core_radius, double r_in, double r_out, double alpha);
    void draw_realmheart_runes(cairo_t* cr, double cx, double cy, double radius, double alpha);
    void draw_core(cairo_t* cr, double cx, double cy, double radius, double alpha, double wallpaper_alpha);
    void draw_radial_slices(cairo_t* cr, double cx, double cy, double r_in, double r_out, double alpha, double wallpaper_alpha);
    void draw_mana_particles(cairo_t* cr, double alpha);
    void draw_reverse_bloom(
        cairo_t* cr,
        double cx,
        double cy,
        double mask_radius,
        double alpha,
        int canvas_width,
        int canvas_height
    );
    void draw_backdrop_dim(cairo_t* cr, double alpha);
    static void draw_pixbuf_cover(cairo_t* cr, GdkPixbuf* pixbuf, double x, double y, double width, double height, double alpha);

    void update_animations(guint64 now_micros);
    void update_particles(guint64 now_micros, double dt);
    void spawn_particle(double x, double y, double vx, double vy, double r, double g, double b, double size, double decay);
    void queue_redraw();
    void start_idle_animation();
    void begin_apply_preparation();
    void begin_apply();
    void accept_apply_preview(
        std::uint64_t generation,
        GdkPixbuf* pixbuf,
        bool fully_opaque,
        std::string error
    );
    void complete_apply_preparation(
        std::uint64_t generation,
        bool success,
        std::string error
    );
    void maybe_begin_prepared_apply(std::uint64_t generation);
    void fail_apply_preparation(std::uint64_t generation, std::string error);
    void complete_apply_commit(
        std::uint64_t generation,
        bool success,
        std::string error
    );
    void maybe_commit_after_cover_presented(GdkFrameClock* frame_clock);
    void handle_apply_cover_presentation_feedback(
        gint64 frame_counter,
        bool complete,
        gint64 presentation_time
    );
    void fire_apply_callback();
    void abort_apply_handoff(std::string error);
    [[nodiscard]] ApplyOutputGeometry current_apply_output_geometry() const noexcept;
    [[nodiscard]] bool apply_output_geometry_matches() const noexcept;
    [[nodiscard]] static int apply_preview_target_dimension(
        int logical_width,
        int logical_height,
        int scale_factor,
        int source_width,
        int source_height
    ) noexcept;
    void finish_apply_if_ready(guint64 now_micros);
    void reset_apply_to_idle();
    void cancel_apply_preparation();
    void begin_dismiss();
    void setup_window(GtkApplication* app);

    void reload_pixbufs();
    void request_preview_load(std::uint64_t generation);
    void schedule_adjacent_prewarm();
    void apply_preview_load(
        std::uint64_t generation,
        int wallpaper_index,
        std::array<GdkPixbuf*, 4>& pixbufs
    );
    void clear_pixbufs();
    void clear_old_pixbufs();

    // Callbacks
    DismissCallback dismiss_callback_;
    ApplyPreparationCallback apply_prepare_callback_;
    ApplyCallback apply_callback_;
    ApplyAbortCallback apply_abort_callback_;
    std::shared_ptr<AsyncState> async_state_ = std::make_shared<AsyncState>();
};

} // namespace realmheart::mana_core