#include "relictombs/ManaCoresSelector.hpp"

#include <cmath>
#include <numbers>
#include <memory>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <string>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include "relictombs/ThumbnailCache.hpp"
#include "ui/LayerSurface.hpp"

namespace realmheart::relictombs {
namespace {

void force_transparent_surface(GtkWidget* widget) {
    GtkNative* native = gtk_widget_get_native(widget);
    if (native == nullptr) return;

    GdkSurface* surface = gtk_native_get_surface(native);
    if (surface == nullptr) return;

    cairo_region_t* empty_region = cairo_region_create();
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_surface_set_opaque_region(surface, empty_region);
    G_GNUC_END_IGNORE_DEPRECATIONS
    cairo_region_destroy(empty_region);
}

void append_annular_sector_path(
    cairo_t* cr,
    double cx, double cy,
    double r_in, double r_out,
    double start_angle, double end_angle
) {
    cairo_arc(cr, cx, cy, r_in, start_angle, end_angle);
    double x_out_end = cx + r_out * std::cos(end_angle);
    double y_out_end = cy + r_out * std::sin(end_angle);
    cairo_line_to(cr, x_out_end, y_out_end);
    cairo_arc_negative(cr, cx, cy, r_out, end_angle, start_angle);
    cairo_close_path(cr);
}

} // namespace

ManaCoresSelector::ManaCoresSelector() = default;

ManaCoresSelector::~ManaCoresSelector() {
    if (tick_callback_id_ != 0 && canvas_ != nullptr) {
        gtk_widget_remove_tick_callback(canvas_, tick_callback_id_);
        tick_callback_id_ = 0;
    }
    if (transparency_retry_id_ != 0 && window_ != nullptr) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(window_), transparency_retry_id_);
        transparency_retry_id_ = 0;
    }
    clear_pixbufs();
    clear_old_pixbufs();
}

void ManaCoresSelector::clear_pixbufs() {
    if (current_core_pixbuf_ != nullptr) {
        g_object_unref(current_core_pixbuf_);
        current_core_pixbuf_ = nullptr;
    }
    for (auto& pb : slice_pixbufs_) {
        if (pb != nullptr) {
            g_object_unref(pb);
            pb = nullptr;
        }
    }
    if (apply_fullscreen_pixbuf_ != nullptr) {
        g_object_unref(apply_fullscreen_pixbuf_);
        apply_fullscreen_pixbuf_ = nullptr;
    }
}

void ManaCoresSelector::clear_old_pixbufs() {
    if (old_core_pixbuf_ != nullptr) {
        g_object_unref(old_core_pixbuf_);
        old_core_pixbuf_ = nullptr;
    }
    for (auto& pb : old_slice_pixbufs_) {
        if (pb != nullptr) {
            g_object_unref(pb);
            pb = nullptr;
        }
    }
}

void ManaCoresSelector::schedule_transparency_retry() {
    if (window_ == nullptr || transparency_retry_id_ != 0) return;
    transparency_retry_count_ = 0;
    transparency_retry_id_ = gtk_widget_add_tick_callback(
        GTK_WIDGET(window_),
        transparency_retry_callback,
        this,
        nullptr
    );
}

gboolean ManaCoresSelector::transparency_retry_callback(GtkWidget* widget, GdkFrameClock* frame_clock, gpointer user_data) {
    (void)frame_clock;
    auto* self = static_cast<ManaCoresSelector*>(user_data);
    if (self == nullptr || self->window_ == nullptr) return G_SOURCE_REMOVE;

    GtkNative* native = gtk_widget_get_native(widget);
    if (native == nullptr) {
        if (++self->transparency_retry_count_ >= 30) {
            self->transparency_retry_id_ = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    GdkSurface* surface = gtk_native_get_surface(native);
    if (surface == nullptr) {
        if (++self->transparency_retry_count_ >= 30) {
            self->transparency_retry_id_ = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    force_transparent_surface(widget);

    if (!gdk_surface_get_mapped(surface)) {
        if (++self->transparency_retry_count_ >= 30) {
            self->transparency_retry_id_ = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    gdk_surface_queue_render(surface);

    if (++self->transparency_retry_count_ >= 30) {
        self->transparency_retry_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void ManaCoresSelector::present(GtkApplication* app) {
    if (visible_) return;
    if (app == nullptr) return;

    visible_ = true;
    state_ = State::Assembling;
    assemble_phase_ = AssemblePhase::Emerge;
    animation_start_micros_ = 0;  // Will be armed on first frame tick
    apply_callback_fired_ = false;
    nav_transitioning_ = false;
    nav_progress_ = 1.0;

    // Initialize layout from monitor dimensions
    GdkDisplay* display = gdk_display_get_default();
    GListModel* monitors = gdk_display_get_monitors(display);
    if (monitors && g_list_model_get_n_items(monitors) > 0) {
        GdkMonitor* monitor = GDK_MONITOR(g_list_model_get_item(monitors, 0));
        if (monitor) {
            GdkRectangle geom;
            gdk_monitor_get_geometry(monitor, &geom);
            layout_ = ManaCoresLayout::for_height(geom.height, geom.width);
            g_object_unref(monitor);
        }
        g_object_unref(monitors);
    } else {
        layout_ = ManaCoresLayout::for_height(1080, 1920);
    }

    // Set initial animation state (emerges from off-screen left)
    current_cx_ = -(layout_.core_radius_small + layout_.slice_depth_attached + 80.0);
    current_cy_ = layout_.core_centre_y;
    current_core_radius_ = layout_.core_radius_small;
    current_slice_r_in_ = layout_.core_radius_small + 2.0;
    current_slice_r_out_ = layout_.core_radius_small + 2.0 + layout_.slice_depth_attached;
    current_slices_ = layout_.attached_slices;
    current_alpha_ = 0.0;
    current_wallpaper_alpha_ = 0.0;

    setup_window(app);

    if (tick_callback_id_ == 0) {
        tick_callback_id_ = gtk_widget_add_tick_callback(canvas_, tick_callback, this, nullptr);
    }

    gtk_widget_set_visible(GTK_WIDGET(window_), TRUE);
    gtk_widget_queue_draw(canvas_);
    schedule_transparency_retry();
}

void ManaCoresSelector::dismiss() {
    visible_ = false;
    state_ = State::Hidden;
    nav_transitioning_ = false;
    nav_progress_ = 1.0;
    if (tick_callback_id_ != 0 && canvas_ != nullptr) {
        gtk_widget_remove_tick_callback(canvas_, tick_callback_id_);
        tick_callback_id_ = 0;
    }
    clear_old_pixbufs();
    if (apply_fullscreen_pixbuf_ != nullptr) {
        g_object_unref(apply_fullscreen_pixbuf_);
        apply_fullscreen_pixbuf_ = nullptr;
    }
    if (window_) {
        gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
    }
    if (dismiss_callback_) {
        dismiss_callback_();
    }
}

void ManaCoresSelector::setup_window(GtkApplication* app) {
    if (window_) return;

    window_ = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_decorated(window_, FALSE);
    gtk_window_set_resizable(window_, TRUE);
    gtk_widget_set_can_target(GTK_WIDGET(window_), FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(window_), "realmheart-mana-cores-window");
    gtk_widget_remove_css_class(GTK_WIDGET(window_), "background");

    // CSS rules for .realmheart-mana-cores-window and .realmheart-mana-cores-canvas
    // are now loaded globally by ThemeStyles at shell startup. Adding a CSS provider
    // dynamically here triggers a global GTK style invalidation which can cause a
    // crash (g_signal_emit -> style-updated) if executed when other shell widgets
    // are in the middle of being realized (like during an early keybind launch).

    ui::LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-mana-cores";
    spec.layer = ui::LayerSurfaceLevel::Overlay;
    spec.keyboard_mode = ui::LayerKeyboardMode::Exclusive;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    spec.anchor_left = true;
    spec.anchor_right = true;
    spec.margin_top = 0;
    spec.margin_bottom = 0;
    spec.margin_left = 0;
    spec.margin_right = 0;
    spec.exclusive_zone = -1;
    spec.monitor_index = -1;

    ui::apply_layer_surface(window_, spec);
    gtk_layer_set_exclusive_zone(window_, -1);

    GtkWidget* root = gtk_overlay_new();
    gtk_widget_add_css_class(root, "realmheart-mana-cores-window");
    gtk_widget_remove_css_class(root, "background");

    g_signal_connect(
        window_,
        "realize",
        G_CALLBACK(+[](GtkWidget* widget, gpointer) {
            force_transparent_surface(widget);
        }),
        nullptr
    );
    g_signal_connect(
        window_,
        "map",
        G_CALLBACK(+[](GtkWidget* widget, gpointer) {
            force_transparent_surface(widget);
        }),
        nullptr
    );

    canvas_ = gtk_drawing_area_new();
    gtk_widget_add_css_class(GTK_WIDGET(canvas_), "realmheart-mana-cores-canvas");
    gtk_widget_remove_css_class(GTK_WIDGET(canvas_), "background");
    gtk_widget_set_visible(canvas_, TRUE);
    gtk_widget_set_hexpand(canvas_, TRUE);
    gtk_widget_set_vexpand(canvas_, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(canvas_), draw_callback, this, nullptr);
    gtk_overlay_set_child(GTK_OVERLAY(root), canvas_);
    gtk_window_set_child(window_, root);
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);

    GtkEventController* key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) -> gboolean {
        auto* self = static_cast<ManaCoresSelector*>(data);
        return self->handle_key(keyval);
    }), this);
    gtk_widget_add_controller(GTK_WIDGET(window_), key_controller);

    g_signal_connect(window_, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
        auto* self = static_cast<ManaCoresSelector*>(data);
        self->dismiss();
        return TRUE;
    }), this);
}

void ManaCoresSelector::set_current_wallpaper(GdkPixbuf* pixbuf) {
    if (current_core_pixbuf_ != nullptr) {
        g_object_unref(current_core_pixbuf_);
    }
    current_core_pixbuf_ = pixbuf ? GDK_PIXBUF(g_object_ref(pixbuf)) : nullptr;
}

void ManaCoresSelector::set_next_wallpapers(std::array<GdkPixbuf*, 3> pixbufs) {
    for (size_t i = 0; i < 3; ++i) {
        if (slice_pixbufs_[i] != nullptr) {
            g_object_unref(slice_pixbufs_[i]);
        }
        slice_pixbufs_[i] = pixbufs[i] ? GDK_PIXBUF(g_object_ref(pixbufs[i])) : nullptr;
    }
}

void ManaCoresSelector::load_wallpapers_from_library(const std::filesystem::path& current_path) {
    WallpaperLibrary library;
    auto discovery = library.discover();
    all_wallpaper_paths_ = discovery.paths;

    current_wallpaper_index_ = 0;
    for (size_t i = 0; i < all_wallpaper_paths_.size(); ++i) {
        if (all_wallpaper_paths_[i] == current_path) {
            current_wallpaper_index_ = static_cast<int>(i);
            break;
        }
    }

    reload_pixbufs();
}

void ManaCoresSelector::reload_pixbufs() {
    if (all_wallpaper_paths_.empty()) return;

    clear_pixbufs();

    const int total = static_cast<int>(all_wallpaper_paths_.size());
    const int core_target_dim = 480;
    const int slice_target_dim = 360;

    // 1. Current Core Wallpaper from ThumbnailCache (fast, non-blocking)
    {
        const auto& path = all_wallpaper_paths_[current_wallpaper_index_];
        std::string error;
        current_core_pixbuf_ = ThumbnailCache::load_or_create(
            path, core_target_dim, &error
        );
    }

    // 2. Three Slices from ThumbnailCache:
    const std::array<int, 3> slice_indices = {
        (current_wallpaper_index_ + 1) % total,
        (current_wallpaper_index_ + 2) % total,
        (current_wallpaper_index_ + 3) % total
    };

    for (size_t i = 0; i < 3; ++i) {
        int idx = slice_indices[i];
        if (idx >= 0 && idx < total) {
            const auto& path = all_wallpaper_paths_[idx];
            slice_pixbufs_[i] = ThumbnailCache::load_or_create(
                path, slice_target_dim, nullptr
            );
        }
    }
}

void ManaCoresSelector::cycle_wallpaper(int direction) {
    if (all_wallpaper_paths_.empty()) return;

    if (nav_transitioning_) {
        nav_transitioning_ = false;
        nav_progress_ = 1.0;
        clear_old_pixbufs();
    }

    if (old_core_pixbuf_ != nullptr) g_object_unref(old_core_pixbuf_);
    old_core_pixbuf_ = current_core_pixbuf_ ? GDK_PIXBUF(g_object_ref(current_core_pixbuf_)) : nullptr;

    for (size_t i = 0; i < 3; ++i) {
        if (old_slice_pixbufs_[i] != nullptr) g_object_unref(old_slice_pixbufs_[i]);
        old_slice_pixbufs_[i] = slice_pixbufs_[i] ? GDK_PIXBUF(g_object_ref(slice_pixbufs_[i])) : nullptr;
    }

    const int total = static_cast<int>(all_wallpaper_paths_.size());
    current_wallpaper_index_ = (current_wallpaper_index_ + direction + total) % total;

    reload_pixbufs();

    // Trigger navigation slide+crossfade
    nav_transitioning_ = true;
    nav_progress_ = 0.0;
    nav_direction_ = direction;
    nav_transition_start_micros_ = g_get_monotonic_time();

    queue_redraw();
}

void ManaCoresSelector::draw_pixbuf_cover(
    cairo_t* cr,
    GdkPixbuf* pixbuf,
    double x, double y,
    double width, double height,
    double alpha
) {
    if (pixbuf == nullptr || width <= 0.0 || height <= 0.0 || alpha <= 0.0) return;

    int pw = gdk_pixbuf_get_width(pixbuf);
    int ph = gdk_pixbuf_get_height(pixbuf);
    if (pw <= 0 || ph <= 0) return;

    double scale = std::max(width / static_cast<double>(pw), height / static_cast<double>(ph));
    double rw = pw * scale;
    double rh = ph * scale;
    double rx = x + (width - rw) * 0.5;
    double ry = y + (height - rh) * 0.5;

    cairo_save(cr);
    cairo_translate(cr, rx, ry);
    cairo_scale(cr, scale, scale);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gdk_cairo_set_source_pixbuf(cr, pixbuf, 0.0, 0.0);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (alpha >= 0.999) {
        cairo_paint(cr);
    } else {
        cairo_paint_with_alpha(cr, alpha);
    }
    cairo_restore(cr);
}

void ManaCoresSelector::draw_cardinal_stars(
    cairo_t* cr,
    double cx, double cy,
    double radius,
    double alpha
) {
    if (alpha <= 0.0 || radius <= 10.0) return;

    const double spike_len = layout_.star_spike_length;
    // 4 Cardinal positions: North (-pi/2), East (0), South (pi/2), West (pi)
    const std::array<double, 4> angles = {
        -std::numbers::pi * 0.5,
        0.0,
        std::numbers::pi * 0.5,
        std::numbers::pi
    };

    for (double theta : angles) {
        double px = cx + radius * std::cos(theta);
        double py = cy + radius * std::sin(theta);

        double cos_t = std::cos(theta);
        double sin_t = std::sin(theta);
        double perp_x = -sin_t;
        double perp_y = cos_t;

        // Diamond vertices
        double tip_out_x = px + cos_t * spike_len;
        double tip_out_y = py + sin_t * spike_len;
        double tip_in_x = px - cos_t * (spike_len * 0.55);
        double tip_in_y = py - sin_t * (spike_len * 0.55);
        double tip_left_x = px + perp_x * (spike_len * 0.45);
        double tip_left_y = py + perp_y * (spike_len * 0.45);
        double tip_right_x = px - perp_x * (spike_len * 0.45);
        double tip_right_y = py - perp_y * (spike_len * 0.45);

        cairo_save(cr);
        cairo_move_to(cr, tip_out_x, tip_out_y);
        cairo_line_to(cr, tip_left_x, tip_left_y);
        cairo_line_to(cr, tip_in_x, tip_in_y);
        cairo_line_to(cr, tip_right_x, tip_right_y);
        cairo_close_path(cr);

        // Bright fill with cyan-white core
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * alpha);
        cairo_fill_preserve(cr);

        // Glow stroke
        cairo_set_source_rgba(cr, 0.85, 0.92, 1.0, 0.8 * alpha);
        cairo_set_line_width(cr, 1.5);
        cairo_stroke(cr);

        // Center sparkle
        cairo_arc(cr, px, py, 2.5, 0, 2.0 * std::numbers::pi);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0 * alpha);
        cairo_fill(cr);

        cairo_restore(cr);
    }
}

void ManaCoresSelector::draw_core(
    cairo_t* cr,
    double cx, double cy,
    double radius,
    double alpha,
    double wallpaper_alpha
) {
    if (radius <= 0.0 || alpha <= 0.0) return;

    // 1. Wallpaper inside core
    if (wallpaper_alpha > 0.0) {
        cairo_save(cr);
        cairo_arc(cr, cx, cy, radius, 0, 2.0 * std::numbers::pi);
        cairo_clip(cr);

        // Curving & rotating slide for navigation
        // Next (dir=+1): new swings in from top slice direction (~ -42° angle),
        //               rotating from tilted to level. Old swings out towards bottom-left with tilt.
        // Prev (dir=-1): inverted directions.
        double dir = static_cast<double>(nav_direction_);
        double box = radius * 2.4;

        if (nav_progress_ < 1.0 && old_core_pixbuf_ != nullptr) {
            // Old image swings out towards bottom-left with slight rotation and fading
            double travel = radius * 0.6;
            double old_progress = nav_progress_;
            double old_dx = -travel * std::cos(42.0 * std::numbers::pi / 180.0) * old_progress * dir;
            double old_dy = travel * std::sin(42.0 * std::numbers::pi / 180.0) * old_progress * dir;
            double old_rot = -0.35 * old_progress * dir;

            cairo_save(cr);
            cairo_translate(cr, cx + old_dx, cy + old_dy);
            cairo_rotate(cr, old_rot);
            draw_pixbuf_cover(
                cr,
                old_core_pixbuf_,
                -box * 0.5, -box * 0.5,
                box, box,
                wallpaper_alpha * alpha * (1.0 - nav_progress_)
            );
            cairo_restore(cr);
        }

        if (current_core_pixbuf_ != nullptr && nav_progress_ > 0.0) {
            // New image swings in from the top slice direction, rotating from angled to level
            double ease = (nav_progress_ < 1.0) ? (1.0 - std::pow(1.0 - nav_progress_, 3.0)) : 1.0;
            double travel = radius * 0.75;
            double in_dx = travel * std::cos(-42.0 * std::numbers::pi / 180.0) * (1.0 - ease) * dir;
            double in_dy = travel * std::sin(-42.0 * std::numbers::pi / 180.0) * (1.0 - ease) * dir;
            // Starts tilted by ~0.4 radians (~23 deg), rotating to 0.0 as it settles
            double in_rot = 0.45 * (1.0 - ease) * dir;

            cairo_save(cr);
            cairo_translate(cr, cx + in_dx, cy + in_dy);
            cairo_rotate(cr, in_rot);
            draw_pixbuf_cover(
                cr,
                current_core_pixbuf_,
                -box * 0.5, -box * 0.5,
                box, box,
                wallpaper_alpha * alpha * (nav_progress_ < 1.0 ? nav_progress_ : 1.0)
            );
            cairo_restore(cr);
        }

        // Subtle dark rim vignette
        cairo_pattern_t* vignette = cairo_pattern_create_radial(
            cx, cy, radius * 0.75,
            cx, cy, radius
        );
        cairo_pattern_add_color_stop_rgba(vignette, 0.0, 0.0, 0.0, 0.0, 0.0);
        cairo_pattern_add_color_stop_rgba(vignette, 1.0, 0.0, 0.0, 0.0, 0.35 * alpha);
        cairo_set_source(cr, vignette);
        cairo_paint(cr);
        cairo_pattern_destroy(vignette);

        cairo_restore(cr);
    }

    const double border = layout_.border_thickness;
    const double glow = layout_.glow_extent;

    // 2. White Mana Core Outer Glow
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.88, 0.94, 1.0, 0.35 * alpha);
    cairo_set_line_width(cr, border + glow);
    cairo_arc(cr, cx, cy, radius, 0, 2.0 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_restore(cr);

    // Inner Glow
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.95, 0.98, 1.0, 0.25 * alpha);
    cairo_set_line_width(cr, border + (glow * 0.5));
    cairo_arc(cr, cx, cy, radius, 0, 2.0 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_restore(cr);

    // Crisp Hard Border
    cairo_save(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * alpha);
    cairo_set_line_width(cr, border);
    cairo_arc(cr, cx, cy, radius, 0, 2.0 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_restore(cr);

    // 3. Four Cardinal Diamond Star Ornaments
    if (wallpaper_alpha > 0.1) {
        draw_cardinal_stars(cr, cx, cy, radius, alpha * wallpaper_alpha);
    }
}

void ManaCoresSelector::draw_radial_slices(
    cairo_t* cr,
    double cx, double cy,
    double r_in, double r_out,
    double alpha,
    double wallpaper_alpha
) {
    if (r_in <= 0.0 || r_out <= r_in || alpha <= 0.0) return;

    const double border = layout_.border_thickness;
    const double glow = layout_.glow_extent;

    for (size_t i = 0; i < 3; ++i) {
        const auto& geom = current_slices_[i];
        const auto& color = layout_.kRadialPalette[i];
        GdkPixbuf* pixbuf = slice_pixbufs_[i];

        double slice_cx = cx;
        double slice_cy = cy;
        double slice_r_in = r_in;
        double slice_r_out = r_out;
        double glow_boost = 1.0;

        // Hover animation for selected radial
        if (state_ == State::Idle && hovered_radial_ == static_cast<int>(i)) {
            double t = static_cast<double>(g_get_monotonic_time() - idle_start_micros_) / 1'000'000.0;
            glow_boost = 1.0 + 0.5 * std::sin(t * 6.0);
            double pop = 14.0 * (layout_.canvas_height / 1080.0);
            slice_cx += pop * std::cos(geom.mid_angle);
            slice_cy += pop * std::sin(geom.mid_angle);
            // Scale slice outward slightly
            slice_r_out += 8.0 * (layout_.canvas_height / 1080.0);
        }

        // 1a. Mana gradient fill (visible when wallpaper is not fully shown)
        double fill_opacity = mana_fill_alpha_ * (1.0 - wallpaper_alpha) * alpha;
        if (fill_opacity > 0.01) {
            cairo_save(cr);
            append_annular_sector_path(
                cr, slice_cx, slice_cy,
                slice_r_in, slice_r_out,
                geom.start_angle, geom.end_angle
            );
            cairo_clip(cr);

            // Radial gradient from slice midpoint outward, coloured by mana palette
            double mid_r = (slice_r_in + slice_r_out) * 0.5;
            double gx = slice_cx + mid_r * std::cos(geom.mid_angle);
            double gy = slice_cy + mid_r * std::sin(geom.mid_angle);
            double grad_r = (slice_r_out - slice_r_in) * 0.85;

            cairo_pattern_t* grad = cairo_pattern_create_radial(gx, gy, 0.0, gx, gy, grad_r);
            // Bright centre fading to deeper tint at edges
            cairo_pattern_add_color_stop_rgba(grad, 0.0,
                std::min(1.0, color[0] + 0.3),
                std::min(1.0, color[1] + 0.3),
                std::min(1.0, color[2] + 0.3),
                0.55 * fill_opacity);
            cairo_pattern_add_color_stop_rgba(grad, 0.6,
                color[0] * 0.7, color[1] * 0.7, color[2] * 0.7,
                0.35 * fill_opacity);
            cairo_pattern_add_color_stop_rgba(grad, 1.0,
                color[0] * 0.4, color[1] * 0.4, color[2] * 0.4,
                0.15 * fill_opacity);
            cairo_set_source(cr, grad);
            cairo_paint(cr);
            cairo_pattern_destroy(grad);

            cairo_restore(cr);
        }

        // 1b. Wallpaper preview inside slice — rotated to fill the sector
        if (wallpaper_alpha > 0.0) {
            cairo_save(cr);
            append_annular_sector_path(
                cr, slice_cx, slice_cy,
                slice_r_in, slice_r_out,
                geom.start_angle, geom.end_angle
            );
            cairo_clip(cr);

            // Midpoint of slice for positioning
            double mid_r = (slice_r_in + slice_r_out) * 0.5;

            // Bounding box tailored to the rotated sector geometry.
            // Since we rotate by mid_angle + 90, the unrotated X axis aligns with
            // the arc (tangent), and the unrotated Y axis aligns with the radius.
            double arc_span = std::abs(geom.end_angle - geom.start_angle);
            double arc_width = slice_r_out * arc_span;
            double radial_depth = slice_r_out - slice_r_in;
            
            // Add slight padding to ensure the curved edges are fully covered
            double bb_w = arc_width * 1.15;
            double bb_h = radial_depth * 1.25;

            // Immersion tweak: slight angular offset per slice slot
            double immersion_tweak = 0.0;
            if (i == 0) immersion_tweak = -0.12;       // ~-7° less
            else if (i == 2) immersion_tweak = 0.12;   // ~+7° more

            // Rotational carousel slide along the curved track:
            // Slices are spaced by delta angle ~42° (0.733 rad).
            // Next (dir=+1): carousel rotates upward along the arc (bot -> mid -> top).
            //   New image arrives from the lower slot (current_angle + delta_angle).
            //   Old image departs towards the upper slot (current_angle - delta_angle).
            // Prev (dir=-1): carousel rotates downward (top -> mid -> bot).
            double s_dir = static_cast<double>(nav_direction_);
            constexpr double delta_angle = 42.0 * std::numbers::pi / 180.0;

            if (nav_progress_ < 1.0 && old_slice_pixbufs_[i] != nullptr) {
                // Old image departs along the curved arc
                double old_angle_offset = -delta_angle * nav_progress_ * s_dir;
                double old_angle = geom.mid_angle + old_angle_offset;
                double old_x = slice_cx + mid_r * std::cos(old_angle);
                double old_y = slice_cy + mid_r * std::sin(old_angle);
                double old_rot = old_angle + (std::numbers::pi / 2.0) + immersion_tweak;

                cairo_save(cr);
                cairo_translate(cr, old_x, old_y);
                cairo_rotate(cr, old_rot);
                draw_pixbuf_cover(
                    cr,
                    old_slice_pixbufs_[i],
                    -bb_w * 0.5, -bb_h * 0.5,
                    bb_w, bb_h,
                    wallpaper_alpha * alpha * (1.0 - nav_progress_)
                );
                cairo_restore(cr);
            }

            if (pixbuf != nullptr && nav_progress_ > 0.0) {
                // New image arrives along the curved arc from the adjacent slot
                double new_angle_offset = 0.0;
                if (nav_progress_ < 1.0) {
                    double ease = 1.0 - std::pow(1.0 - nav_progress_, 3.0);
                    new_angle_offset = delta_angle * (1.0 - ease) * s_dir;
                }
                double cur_angle = geom.mid_angle + new_angle_offset;
                double cur_x = slice_cx + mid_r * std::cos(cur_angle);
                double cur_y = slice_cy + mid_r * std::sin(cur_angle);
                double cur_rot = cur_angle + (std::numbers::pi / 2.0) + immersion_tweak;

                cairo_save(cr);
                cairo_translate(cr, cur_x, cur_y);
                cairo_rotate(cr, cur_rot);
                draw_pixbuf_cover(
                    cr,
                    pixbuf,
                    -bb_w * 0.5, -bb_h * 0.5,
                    bb_w, bb_h,
                    wallpaper_alpha * alpha * (nav_progress_ < 1.0 ? nav_progress_ : 1.0)
                );
                cairo_restore(cr);
            }

            // Subtle dark tint to contrast border glow
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.18 * alpha);
            cairo_paint(cr);

            cairo_restore(cr);
        }

        // 2. Coloured border & glow
        cairo_save(cr);

        // Outer glow
        append_annular_sector_path(
            cr, slice_cx, slice_cy,
            slice_r_in, slice_r_out,
            geom.start_angle, geom.end_angle
        );
        cairo_set_source_rgba(cr, color[0], color[1], color[2], 0.38 * alpha * glow_boost);
        cairo_set_line_width(cr, border + glow * glow_boost);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);

        // Crisp border
        append_annular_sector_path(
            cr, slice_cx, slice_cy,
            slice_r_in, slice_r_out,
            geom.start_angle, geom.end_angle
        );
        cairo_set_source_rgba(cr, color[0], color[1], color[2], 0.95 * alpha);
        cairo_set_line_width(cr, border);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);

        cairo_restore(cr);
    }
}

void ManaCoresSelector::draw_reverse_bloom(
    cairo_t* cr,
    double cx, double cy,
    double mask_radius
) {
    if (apply_fullscreen_pixbuf_ == nullptr) return;

    cairo_save(cr);

    // Clip to region OUTSIDE the shrinking circle hole
    cairo_rectangle(cr, 0, 0, layout_.canvas_width, layout_.canvas_height);
    if (mask_radius > 1.0) {
        cairo_arc_negative(cr, cx, cy, mask_radius, 2.0 * std::numbers::pi, 0.0);
    }
    cairo_clip(cr);

    // Render fullscreen new wallpaper
    draw_pixbuf_cover(
        cr,
        apply_fullscreen_pixbuf_,
        0, 0,
        layout_.canvas_width, layout_.canvas_height,
        1.0
    );

    // Glowing border along the inner reveal edge
    if (mask_radius > 6.0) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
        cairo_set_line_width(cr, 3.0);
        cairo_arc(cr, cx, cy, mask_radius, 0, 2.0 * std::numbers::pi);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, 0.82, 0.92, 1.0, 0.35);
        cairo_set_line_width(cr, 18.0);
        cairo_arc(cr, cx, cy, mask_radius, 0, 2.0 * std::numbers::pi);
        cairo_stroke(cr);
    }

    cairo_restore(cr);
}



void ManaCoresSelector::draw_backdrop_dim(
    cairo_t* cr,
    double alpha
) {
    if (alpha <= 0.01) return;
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.02, 0.45 * alpha);
    cairo_paint(cr);
    cairo_restore(cr);
}

void ManaCoresSelector::draw(GtkDrawingArea*, cairo_t* cr, int, int) {
    if (!visible_) return;

    // Clear buffer to fully transparent
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // In Applying phase: draw reverse radial bloom first
    if (state_ == State::Applying && apply_mask_radius_ >= 0.0) {
        draw_reverse_bloom(cr, layout_.core_centre_x, layout_.core_centre_y, apply_mask_radius_);
    }

    // Draw central core and radial slices
    if (current_alpha_ > 0.0) {
        // Backdrop dim — subtle dark overlay so the selector pops
        draw_backdrop_dim(cr, current_alpha_ * current_wallpaper_alpha_);



        draw_core(
            cr,
            current_cx_, current_cy_,
            current_core_radius_,
            current_alpha_,
            current_wallpaper_alpha_
        );

        draw_radial_slices(
            cr,
            current_cx_, current_cy_,
            current_slice_r_in_, current_slice_r_out_,
            current_alpha_,
            current_wallpaper_alpha_
        );
    }
}

void ManaCoresSelector::draw_callback(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer user_data) {
    auto* self = static_cast<ManaCoresSelector*>(user_data);
    self->draw(area, cr, width, height);
}

gboolean ManaCoresSelector::tick_callback(GtkWidget*, GdkFrameClock*, gpointer user_data) {
    auto* self = static_cast<ManaCoresSelector*>(user_data);
    if (!self->visible_) return G_SOURCE_REMOVE;

    guint64 now = g_get_monotonic_time();
    if (self->animation_start_micros_ == 0) {
        self->animation_start_micros_ = now;
    }
    self->update_animations(now);
    return G_SOURCE_CONTINUE;
}

void ManaCoresSelector::update_animations(guint64 now_micros) {
    if (state_ == State::Assembling) {
        const double elapsed = static_cast<double>(now_micros - animation_start_micros_) / 1'000'000.0;

        switch (assemble_phase_) {
        case AssemblePhase::Emerge: {
            // Emerge: small glowing core slides from off-screen left to center (360ms)
            constexpr double kEmergeDuration = 0.36;
            double progress = std::min(elapsed / kEmergeDuration, 1.0);
            // Cubic ease-out
            double ease = 1.0 - std::pow(1.0 - progress, 3.0);

            double start_x = -(layout_.core_radius_small + layout_.slice_depth_attached + 80.0);
            current_cx_ = start_x + (layout_.core_centre_x - start_x) * ease;
            current_cy_ = layout_.core_centre_y;
            current_core_radius_ = layout_.core_radius_small;
            current_slice_r_in_ = layout_.core_radius_small + 3.0;
            current_slice_r_out_ = current_slice_r_in_ + layout_.slice_depth_attached;
            current_slices_ = layout_.attached_slices;
            current_alpha_ = std::min(ease * 1.5, 1.0);
            current_wallpaper_alpha_ = 0.0;
            mana_fill_alpha_ = 0.0;

            if (progress >= 1.0) {
                assemble_phase_ = AssemblePhase::Formation;
                animation_start_micros_ = now_micros;
            }
            break;
        }
        case AssemblePhase::Formation: {
            // Formation: Three slices attach to the right side of the core (160ms)
            constexpr double kFormationDuration = 0.16;
            double progress = std::min(elapsed / kFormationDuration, 1.0);

            current_cx_ = layout_.core_centre_x;
            current_cy_ = layout_.core_centre_y;
            current_core_radius_ = layout_.core_radius_small;
            current_slice_r_in_ = layout_.core_radius_small + 3.0;
            current_slice_r_out_ = current_slice_r_in_ + layout_.slice_depth_attached;
            current_slices_ = layout_.attached_slices;
            current_alpha_ = 1.0;
            current_wallpaper_alpha_ = 0.0;
            mana_fill_alpha_ = progress;

            if (progress >= 1.0) {
                assemble_phase_ = AssemblePhase::Expansion;
                animation_start_micros_ = now_micros;
            }
            break;
        }
        case AssemblePhase::Expansion: {
            // Expansion: Core expands to full radius; slices detach and move to right parked radius (480ms)
            constexpr double kExpansionDuration = 0.48;
            double progress = std::min(elapsed / kExpansionDuration, 1.0);
            // Quartic ease-out
            double ease = 1.0 - std::pow(1.0 - progress, 4.0);

            current_cx_ = layout_.core_centre_x;
            current_cy_ = layout_.core_centre_y;

            // Expand core
            current_core_radius_ = layout_.core_radius_small +
                (layout_.core_radius_expanded - layout_.core_radius_small) * ease;

            // Detach slices outward to the right
            double target_r_in = layout_.core_radius_expanded + layout_.slice_gap;
            double target_r_out = target_r_in + layout_.slice_depth_expanded;
            double start_r_in = layout_.core_radius_small + 3.0;
            double start_r_out = start_r_in + layout_.slice_depth_attached;

            current_slice_r_in_ = start_r_in + (target_r_in - start_r_in) * ease;
            current_slice_r_out_ = start_r_out + (target_r_out - start_r_out) * ease;

            // Interpolate slice angles from attached (120° encompassing) to detached (36° parked right)
            for (size_t i = 0; i < 3; ++i) {
                current_slices_[i].start_angle = layout_.attached_slices[i].start_angle +
                    (layout_.detached_slices[i].start_angle - layout_.attached_slices[i].start_angle) * ease;
                current_slices_[i].end_angle = layout_.attached_slices[i].end_angle +
                    (layout_.detached_slices[i].end_angle - layout_.attached_slices[i].end_angle) * ease;
                current_slices_[i].mid_angle = layout_.attached_slices[i].mid_angle +
                    (layout_.detached_slices[i].mid_angle - layout_.attached_slices[i].mid_angle) * ease;
            }

            current_alpha_ = 1.0;
            current_wallpaper_alpha_ = ease;
            mana_fill_alpha_ = 1.0 - ease;

            if (progress >= 1.0) {
                mana_fill_alpha_ = 0.0;
                current_wallpaper_alpha_ = 1.0;
                current_core_radius_ = layout_.core_radius_expanded;
                state_ = State::Idle;
                idle_start_micros_ = now_micros;
                start_idle_animation();
            }
            break;
        }
        }

        queue_redraw();
    } else if (state_ == State::Idle) {
        current_cx_ = layout_.core_centre_x;
        current_cy_ = layout_.core_centre_y;
        current_core_radius_ = layout_.core_radius_expanded;
        current_slice_r_in_ = layout_.core_radius_expanded + layout_.slice_gap;
        current_slice_r_out_ = current_slice_r_in_ + layout_.slice_depth_expanded;
        current_slices_ = layout_.detached_slices;
        current_alpha_ = 1.0;
        current_wallpaper_alpha_ = 1.0;
        mana_fill_alpha_ = 0.0;

        // Navigation slide + crossfade
        if (nav_transitioning_) {
            constexpr double kNavDuration = 0.22;  // Silky, fast 220ms
            double nav_elapsed = static_cast<double>(now_micros - nav_transition_start_micros_) / 1'000'000.0;
            nav_progress_ = std::min(nav_elapsed / kNavDuration, 1.0);
            if (nav_progress_ >= 1.0) {
                nav_transitioning_ = false;
                nav_progress_ = 1.0;
                clear_old_pixbufs();
            }
        }

        queue_redraw();
    } else if (state_ == State::Applying) {
        const double elapsed = static_cast<double>(now_micros - apply_start_micros_) / 1'000'000.0;
        constexpr double kApplyDuration = 0.65;  // 650ms total apply animation

        double progress = std::min(elapsed / kApplyDuration, 1.0);

        // 1. Reverse radial bloom mask radius (shrinks from screen diagonal down to 0)
        double diag = std::hypot(layout_.canvas_width, layout_.canvas_height);
        double bloom_ease = progress * (2.0 - progress);
        apply_mask_radius_ = diag * (1.0 - bloom_ease);

        // 2. Core contraction & slices re-attaching (0.0 to 0.45s)
        double contract_p = std::min(progress / 0.70, 1.0);
        double contract_ease = contract_p * (2.0 - contract_p);

        current_core_radius_ = layout_.core_radius_expanded -
            (layout_.core_radius_expanded - layout_.core_radius_shrunk) * contract_ease;

        double target_r_in = layout_.core_radius_shrunk + 2.0;
        double target_r_out = target_r_in + layout_.slice_depth_attached;
        double start_r_in = layout_.core_radius_expanded + layout_.slice_gap;
        double start_r_out = start_r_in + layout_.slice_depth_expanded;

        current_slice_r_in_ = start_r_in - (start_r_in - target_r_in) * contract_ease;
        current_slice_r_out_ = start_r_out - (start_r_out - target_r_out) * contract_ease;

        for (size_t i = 0; i < 3; ++i) {
            current_slices_[i].start_angle = layout_.detached_slices[i].start_angle + (layout_.attached_slices[i].start_angle - layout_.detached_slices[i].start_angle) * contract_ease;
            current_slices_[i].end_angle = layout_.detached_slices[i].end_angle + (layout_.attached_slices[i].end_angle - layout_.detached_slices[i].end_angle) * contract_ease;
            current_slices_[i].mid_angle = layout_.detached_slices[i].mid_angle + (layout_.attached_slices[i].mid_angle - layout_.detached_slices[i].mid_angle) * contract_ease;
        }

        // 3. Slide back to offscreen left (0.45s to 0.65s)
        if (progress > 0.60) {
            double slide_p = (progress - 0.60) / 0.40;
            double slide_ease = slide_p * slide_p;
            current_cx_ = layout_.core_centre_x - (layout_.core_centre_x + 200.0) * slide_ease;
            current_alpha_ = 1.0 - slide_p;
        } else {
            current_cx_ = layout_.core_centre_x;
            current_alpha_ = 1.0;
        }

        // 4. Trigger wallpaper commit at 85% progress
        if (progress >= 0.85 && !apply_callback_fired_) {
            apply_callback_fired_ = true;
            if (!all_wallpaper_paths_.empty() && current_wallpaper_index_ >= 0 &&
                current_wallpaper_index_ < static_cast<int>(all_wallpaper_paths_.size())) {
                std::string path = all_wallpaper_paths_[current_wallpaper_index_].string();
                if (apply_callback_) {
                    apply_callback_(path);
                }
            }
        }

        if (progress >= 1.0) {
            state_ = State::Hidden;
            dismiss();
        }

        queue_redraw();
    } else if (state_ == State::Dismissing) {
        const double elapsed = static_cast<double>(now_micros - dismiss_start_micros_) / 1'000'000.0;

        switch (dismiss_phase_) {
        case DismissPhase::Contraction: {
            // Slices re-attach + core shrinks (350ms)
            constexpr double kContractDuration = 0.35;
            double progress = std::min(elapsed / kContractDuration, 1.0);
            double ease = 1.0 - std::pow(1.0 - progress, 3);  // cubic ease-out

            current_cx_ = layout_.core_centre_x;
            current_cy_ = layout_.core_centre_y;

            // Shrink core
            current_core_radius_ = layout_.core_radius_expanded -
                (layout_.core_radius_expanded - layout_.core_radius_small) * ease;

            // Re-attach slices
            double start_r_in = layout_.core_radius_expanded + layout_.slice_gap;
            double start_r_out = start_r_in + layout_.slice_depth_expanded;
            double target_r_in = layout_.core_radius_small + 2.0;
            double target_r_out = target_r_in + layout_.slice_depth_attached;

            current_slice_r_in_ = start_r_in + (target_r_in - start_r_in) * ease;
            current_slice_r_out_ = start_r_out + (target_r_out - start_r_out) * ease;

            for (size_t i = 0; i < 3; ++i) {
                current_slices_[i].start_angle = layout_.detached_slices[i].start_angle + (layout_.attached_slices[i].start_angle - layout_.detached_slices[i].start_angle) * ease;
                current_slices_[i].end_angle = layout_.detached_slices[i].end_angle + (layout_.attached_slices[i].end_angle - layout_.detached_slices[i].end_angle) * ease;
                current_slices_[i].mid_angle = layout_.detached_slices[i].mid_angle + (layout_.attached_slices[i].mid_angle - layout_.detached_slices[i].mid_angle) * ease;
            }

            current_alpha_ = 1.0;
            current_wallpaper_alpha_ = 1.0 - ease;  // Fade out wallpaper
            mana_fill_alpha_ = ease;                 // Fade in mana fill

            if (progress >= 1.0) {
                dismiss_phase_ = DismissPhase::Slide;
                dismiss_start_micros_ = now_micros;
            }
            break;
        }
        case DismissPhase::Slide: {
            // Slide off screen left (250ms)
            constexpr double kSlideDuration = 0.25;
            double progress = std::min(elapsed / kSlideDuration, 1.0);
            double ease = progress * progress;  // ease-in (accelerating)

            current_cx_ = layout_.core_centre_x - (layout_.core_centre_x + 200.0) * ease;
            current_alpha_ = 1.0 - progress;
            current_wallpaper_alpha_ = 0.0;
            mana_fill_alpha_ = 1.0;

            if (progress >= 1.0) {
                state_ = State::Hidden;
                dismiss();
            }
            break;
        }
        }

        queue_redraw();
    }
}

void ManaCoresSelector::start_idle_animation() {
    idle_start_micros_ = g_get_monotonic_time();
}

void ManaCoresSelector::request_apply() {
    if (state_ != State::Idle) return;
    begin_apply();
}

void ManaCoresSelector::force_apply(const std::string& wallpaper_path) {
    // If a specific wallpaper path is requested, switch to it and apply
    for (size_t i = 0; i < all_wallpaper_paths_.size(); ++i) {
        if (all_wallpaper_paths_[i] == wallpaper_path) {
            current_wallpaper_index_ = static_cast<int>(i);
            reload_pixbufs();
            break;
        }
    }
    begin_apply();
}

void ManaCoresSelector::begin_apply() {
    state_ = State::Applying;
    if (nav_transitioning_) {
        nav_transitioning_ = false;
        nav_progress_ = 1.0;
        clear_old_pixbufs();
    }
    apply_start_micros_ = g_get_monotonic_time();
    apply_callback_fired_ = false;
    apply_mask_radius_ = std::hypot(layout_.canvas_width, layout_.canvas_height);

    // Lazily load the full high-resolution image only when the user applies
    if (!all_wallpaper_paths_.empty() && current_wallpaper_index_ >= 0 &&
        current_wallpaper_index_ < static_cast<int>(all_wallpaper_paths_.size())) {
        if (apply_fullscreen_pixbuf_ != nullptr) {
            g_object_unref(apply_fullscreen_pixbuf_);
            apply_fullscreen_pixbuf_ = nullptr;
        }
        const auto& path = all_wallpaper_paths_[current_wallpaper_index_];
        apply_fullscreen_pixbuf_ = gdk_pixbuf_new_from_file_at_scale(
            path.c_str(), static_cast<int>(layout_.canvas_width), -1, TRUE, nullptr
        );
    }

    queue_redraw();
}

void ManaCoresSelector::begin_dismiss() {
    state_ = State::Dismissing;
    dismiss_phase_ = DismissPhase::Contraction;
    dismiss_start_micros_ = g_get_monotonic_time();
    queue_redraw();
}

bool ManaCoresSelector::handle_key(guint keyval) {
    if (state_ == State::Idle) {
        switch (keyval) {
        case GDK_KEY_Escape:
        case GDK_KEY_q:
        case GDK_KEY_Q:
            begin_dismiss();
            return true;

        case GDK_KEY_Left:
        case GDK_KEY_h:
        case GDK_KEY_H:
            cycle_wallpaper(-1);
            return true;

        case GDK_KEY_Right:
        case GDK_KEY_l:
        case GDK_KEY_L:
            cycle_wallpaper(1);
            return true;

        case GDK_KEY_Up:
        case GDK_KEY_k:
        case GDK_KEY_K:
            if (hovered_radial_ > 0) {
                hovered_radial_--;
            } else if (hovered_radial_ == -1) {
                hovered_radial_ = 0;
            }
            queue_redraw();
            return true;

        case GDK_KEY_Down:
        case GDK_KEY_j:
        case GDK_KEY_J:
            if (hovered_radial_ < 2 && hovered_radial_ >= 0) {
                hovered_radial_++;
            } else if (hovered_radial_ == -1) {
                hovered_radial_ = 2;
            }
            queue_redraw();
            return true;

        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
        case GDK_KEY_space:
            request_apply();
            return true;

        default:
            break;
        }
    } else if (state_ == State::Assembling) {
        if (keyval == GDK_KEY_Escape || keyval == GDK_KEY_q || keyval == GDK_KEY_Q) {
            begin_dismiss();
            return true;
        }
    }
    return false;
}

void ManaCoresSelector::queue_redraw() {
    if (canvas_) {
        gtk_widget_queue_draw(canvas_);
    }
}

} // namespace realmheart::relictombs