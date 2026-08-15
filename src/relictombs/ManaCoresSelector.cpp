#include "relictombs/ManaCoresSelector.hpp"

#include <cmath>
#include <numbers>
#include <memory>
#include <iostream>
#include <filesystem>
#include <string>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include "ui/LayerSurface.hpp"

namespace realmheart::relictombs {

ManaCoresSelector::ManaCoresSelector() = default;

ManaCoresSelector::~ManaCoresSelector() {
    if (tick_callback_id_ != 0) {
        gtk_widget_remove_tick_callback(canvas_, tick_callback_id_);
        tick_callback_id_ = 0;
    }
}

void ManaCoresSelector::present(GtkApplication* app) {
    if (visible_) return;

    visible_ = true;
    state_ = State::Assembling;
    assemble_phase_ = AssemblePhase::CoreIn;
    animation_start_micros_ = g_get_monotonic_time();
    core_current_radius_ = 0.0;

    // Initialize layout from the primary monitor's dimensions
    if (layout_.canvas_width == 0.0 || layout_.canvas_height == 0.0) {
        GdkDisplay* display = gdk_display_get_default();
        GListModel* monitors = gdk_display_get_monitors(display);
        if (monitors && g_list_model_get_n_items(monitors) > 0) {
            GdkMonitor* monitor = GDK_MONITOR(g_list_model_get_item(monitors, 0));
            if (monitor) {
                GdkRectangle geom;
                gdk_monitor_get_geometry(monitor, &geom);
                layout_ = ManaCoresLayout::for_height(geom.height);
                g_object_unref(monitor);
            }
            g_object_unref(monitors);
        }
    }

    // Initialize radial targets to their fan-in start positions (off-screen left)
    const double cx = layout_.core_centre_x;
    const double cy = layout_.core_centre_y;
    const double fan_r = layout_.fan_arc_radius;
    const double start_angle = layout_.fan_start_angle;
    const double angle_step = (layout_.fan_end_angle - layout_.fan_start_angle) / 3.0;

    for (int i = 0; i < 3; ++i) {
        double angle = start_angle + i * angle_step;
        radial_target_x_[i] = cx + fan_r * std::cos(angle);
        radial_target_y_[i] = cy + fan_r * std::sin(angle);
        radial_progress_[i] = 0.0;
    }

    // Create window and layer surface if not exists
    setup_window(app);

    // Add tick callback for animations
    if (tick_callback_id_ == 0) {
        tick_callback_id_ = gtk_widget_add_tick_callback(canvas_, tick_callback, this, nullptr);
    }

    // Present the window
    gtk_window_present(window_);

    queue_redraw();
}

void ManaCoresSelector::dismiss() {
    visible_ = false;
    state_ = State::Hidden;
    if (tick_callback_id_ != 0 && canvas_ != nullptr) {
        gtk_widget_remove_tick_callback(canvas_, tick_callback_id_);
        tick_callback_id_ = 0;
    }
    if (window_) {
        gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
    }
    // Fire dismiss callback to let shell restore workspace + bar
    if (dismiss_callback_) {
        dismiss_callback_();
    }
}

void ManaCoresSelector::setup_window(GtkApplication* app) {
    if (window_) return;

    window_ = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_decorated(window_, FALSE);
    gtk_window_set_resizable(window_, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(window_), "realmheart-mana-cores-window");
    gtk_widget_remove_css_class(GTK_WIDGET(window_), "background");

    // Apply layer surface with full output coverage
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
    spec.monitor_index = 0;  // Explicitly set monitor index

    ui::apply_layer_surface(window_, spec);
    gtk_layer_set_exclusive_zone(window_, -1);

    // Establish an empty opaque region as soon as the Wayland surface exists,
    // and repeat it on map because GTK may recompute opaque regions while the
    // widget tree changes between hidden and visible states.
    g_signal_connect(
        window_,
        "realize",
        G_CALLBACK(+[](GtkWidget* widget, gpointer) {
            GtkNative* native = gtk_widget_get_native(widget);
            if (native) {
                GdkSurface* surface = gtk_native_get_surface(native);
                if (surface) {
                    cairo_region_t* empty_region = cairo_region_create();
                    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                    gdk_surface_set_opaque_region(surface, empty_region);
                    G_GNUC_END_IGNORE_DEPRECATIONS
                    cairo_region_destroy(empty_region);
                }
            }
        }),
        nullptr
    );
    g_signal_connect(
        window_,
        "map",
        G_CALLBACK(+[](GtkWidget* widget, gpointer) {
            GtkNative* native = gtk_widget_get_native(widget);
            if (native) {
                GdkSurface* surface = gtk_native_get_surface(native);
                if (surface) {
                    cairo_region_t* empty_region = cairo_region_create();
                    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                    gdk_surface_set_opaque_region(surface, empty_region);
                    G_GNUC_END_IGNORE_DEPRECATIONS
                    cairo_region_destroy(empty_region);
                }
            }
        }),
        nullptr
    );

    // Create canvas BEFORE presenting the window
    canvas_ = gtk_drawing_area_new();
    gtk_widget_remove_css_class(GTK_WIDGET(canvas_), "background");
    gtk_widget_set_visible(canvas_, TRUE);
    gtk_widget_set_hexpand(canvas_, TRUE);
    gtk_widget_set_vexpand(canvas_, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(canvas_), draw_callback, this, nullptr);
    gtk_window_set_child(window_, canvas_);

    // NOW present the window after canvas is set as child
    // (present() will call gtk_window_present after setup_window returns)
    // Connect keyboard events
    GtkEventController* key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) -> gboolean {
        auto* self = static_cast<ManaCoresSelector*>(data);
        return self->handle_key(keyval);
    }), this);
    gtk_widget_add_controller(GTK_WIDGET(window_), key_controller);

    // Connect close signal
    g_signal_connect(window_, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
        auto* self = static_cast<ManaCoresSelector*>(data);
        self->dismiss();
        return TRUE;  // Prevent default close
    }), this);
}

void ManaCoresSelector::set_current_wallpaper(GdkPixbuf* pixbuf) {
    current_wallpaper_ = pixbuf;
}

void ManaCoresSelector::set_next_wallpapers(std::array<GdkPixbuf*, 3> pixbufs) {
    next_wallpapers_ = pixbufs;
}

void ManaCoresSelector::load_wallpapers_from_library(const std::filesystem::path& current_path) {
    WallpaperLibrary library;
    auto discovery = library.discover();
    
    // Find the current wallpaper index in the discovered paths
    int current_idx = 0;
    for (size_t i = 0; i < discovery.paths.size(); ++i) {
        if (discovery.paths[i] == current_path) {
            current_idx = static_cast<int>(i);
            break;
        }
    }
    
    current_wallpaper_index_ = current_idx;
    
    // Load the CURRENT wallpaper for the core
    {
        GError* error = nullptr;
        double scale = layout_.canvas_height / 1080.0;
        int target_size = static_cast<int>(layout_.core_radius * 2 * scale);
        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_scale(
            current_path.string().c_str(),
            target_size, target_size, TRUE, &error
        );
        if (pixbuf != nullptr) {
            current_wallpaper_ = pixbuf;
        } else if (error) {
            g_error_free(error);
        }
    }
    
    // Load the next 3 wallpapers as pixbufs
    for (int i = 0; i < 3; ++i) {
        int idx = (current_idx + 1 + i) % static_cast<int>(discovery.paths.size());
        if (idx < 0 || idx >= static_cast<int>(discovery.paths.size())) {
            next_wallpaper_paths_[i].clear();
            next_wallpapers_[i] = nullptr;
            continue;
        }
        
        next_wallpaper_paths_[i] = discovery.paths[idx].string();
        // Load pixbuf - cover-crop at radial display size
        GError* error = nullptr;
        double scale = layout_.canvas_height / 1080.0;
        int target_size = static_cast<int>(layout_.radial_radius * 2 * scale);
        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_scale(
            discovery.paths[idx].string().c_str(),
            target_size, target_size, TRUE, &error
        );
        if (pixbuf != nullptr) {
            next_wallpapers_[i] = pixbuf;
        } else {
            g_error_free(error);
            next_wallpapers_[i] = nullptr;
        }
    }
}

void ManaCoresSelector::cycle_wallpaper(int direction) {
    // Cycle the wallpaper selection: shift the 3 previews by `direction`
    // direction: -1 = left (previous), +1 = right (next)
    if (next_wallpaper_paths_[0].empty()) return;
    
    // Reload wallpapers shifted by direction
    WallpaperLibrary library;
    auto discovery = library.discover();
    
    // Adjust current index
    current_wallpaper_index_ = (current_wallpaper_index_ + direction + 
        static_cast<int>(discovery.paths.size())) % static_cast<int>(discovery.paths.size());
    
    // Reload CURRENT wallpaper for core
    const std::filesystem::path& new_current_path = discovery.paths[current_wallpaper_index_];
    {
        GError* error = nullptr;
        double scale = layout_.canvas_height / 1080.0;
        int target_size = static_cast<int>(layout_.core_radius * 2 * scale);
        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_scale(
            new_current_path.string().c_str(),
            target_size, target_size, TRUE, &error
        );
        if (pixbuf != nullptr) {
            if (current_wallpaper_ != nullptr) {
                g_object_unref(current_wallpaper_);
            }
            current_wallpaper_ = pixbuf;
        } else if (error) {
            g_error_free(error);
        }
    }
    
    // Reload next 3
    for (int i = 0; i < 3; ++i) {
        int idx = (current_wallpaper_index_ + 1 + i) % static_cast<int>(discovery.paths.size());
        next_wallpaper_paths_[i] = discovery.paths[idx].string();
        
        GError* error = nullptr;
        double scale = layout_.canvas_height / 1080.0;
        int target_size = static_cast<int>(layout_.radial_radius * 2 * scale);
        GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_scale(
            discovery.paths[idx].string().c_str(),
            target_size, target_size, TRUE, &error
        );
        if (pixbuf != nullptr) {
            // Free old pixbuf
            if (next_wallpapers_[i] != nullptr) {
                g_object_unref(next_wallpapers_[i]);
            }
            next_wallpapers_[i] = pixbuf;
        } else {
            g_error_free(error);
        }
    }
    
    queue_redraw();
}

void ManaCoresSelector::force_apply(const std::string& /*wallpaper_path*/) {
    // Apply a specific wallpaper immediately (bypass selection)
    begin_apply();
    // The apply callback will fire when the animation completes
}

void ManaCoresSelector::draw_core(cairo_t* cr, double alpha) {
    if (alpha <= 0.0) return;

    const double cx = layout_.core_centre_x;
    const double cy = layout_.core_centre_y;
    const double r = layout_.core_radius;
    const double border = layout_.border_thickness;
    const double glow = layout_.glow_extent;

    // Outer glow - white with soft falloff
    cairo_save(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.35 * alpha);
    cairo_set_line_width(cr, border + glow);
    cairo_arc(cr, cx, cy, r, 0, 2 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_restore(cr);

    // Hard border - crisp white
    cairo_save(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * alpha);
    cairo_set_line_width(cr, border);
    cairo_arc(cr, cx, cy, r, 0, 2 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void ManaCoresSelector::draw_core_animated(cairo_t* cr, double alpha, double radius, double cx, double cy) {
    if (alpha <= 0.0 || radius <= 0.0) return;

    const double border = layout_.border_thickness;
    const double glow = layout_.glow_extent;

    // Outer glow - white with soft falloff
    cairo_save(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.35 * alpha);
    cairo_set_line_width(cr, border + glow);
    cairo_arc(cr, cx, cy, radius, 0, 2 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_restore(cr);

    // Hard border - crisp white
    cairo_save(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95 * alpha);
    cairo_set_line_width(cr, border);
    cairo_arc(cr, cx, cy, radius, 0, 2 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void ManaCoresSelector::draw_radials(cairo_t* cr, double alpha) {
    if (alpha <= 0.0) return;

    const double border = layout_.border_thickness;
    const double glow = layout_.glow_extent;
    const double r = layout_.radial_radius;
    const double base_x = layout_.radials_park_x;
    const double base_y = layout_.radials_park_y;
    const double spacing = layout_.radial_spacing;

    for (int i = 0; i < 3; ++i) {
        // Apply idle floating offsets
        double offset_x = 0.0, offset_y = 0.0;
        if (state_ == State::Idle && idle_start_micros_ > 0) {
            double t = static_cast<double>(g_get_monotonic_time() - idle_start_micros_) / 1'000'000.0;
            const auto& params = idle_float_params_[i];
            offset_x = params.amplitude_x * std::sin(2.0 * std::numbers::pi * t / params.period_x + params.phase_x);
            offset_y = params.amplitude_y * std::sin(2.0 * std::numbers::pi * t / params.period_y + params.phase_y);
        }

        // Apply hover animation
        double hover_scale = 1.0;
        double hover_glow_boost = 1.0;
        if (i == hovered_radial_ && state_ == State::Idle) {
            double t = static_cast<double>(g_get_monotonic_time() - idle_start_micros_) / 1'000'000.0;
            hover_scale = 1.1;  // 10% scale up
            hover_glow_boost = 1.0 + 0.2 * std::sin(t * 8.0);  // Pulse at 8 Hz
        }

        const double cx = base_x + offset_x;
        const double cy = base_y + (i - 1) * spacing + offset_y;
        const double draw_r = r * hover_scale;
        const auto& colour = layout_.kRadialPalette[i];

        // Outer glow - radial's mana colour with hover boost
        cairo_save(cr);
        cairo_set_source_rgba(cr, colour[0], colour[1], colour[2], 0.35 * alpha * hover_glow_boost);
        cairo_set_line_width(cr, border + glow * hover_glow_boost);
        cairo_arc(cr, cx, cy, draw_r, 0, 2 * std::numbers::pi);
        cairo_stroke(cr);
        cairo_restore(cr);

        // Hard border
        cairo_save(cr);
        cairo_set_source_rgba(cr, colour[0], colour[1], colour[2], 0.95 * alpha);
        cairo_set_line_width(cr, border);
        cairo_arc(cr, cx, cy, draw_r, 0, 2 * std::numbers::pi);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
}

void ManaCoresSelector::draw_radials_animated(cairo_t* cr, double alpha) {
    if (alpha <= 0.0) return;

    const double border = layout_.border_thickness;
    const double glow = layout_.glow_extent;
    const double r = layout_.radial_radius;

    for (int i = 0; i < 3; ++i) {
        const double cx = radial_target_x_[i];
        const double cy = radial_target_y_[i];
        const auto& colour = layout_.kRadialPalette[i];

        // Outer glow - radial's mana colour
        cairo_save(cr);
        cairo_set_source_rgba(cr, colour[0], colour[1], colour[2], 0.35 * alpha);
        cairo_set_line_width(cr, border + glow);
        cairo_arc(cr, cx, cy, r, 0, 2 * std::numbers::pi);
        cairo_stroke(cr);
        cairo_restore(cr);

        // Hard border
        cairo_save(cr);
        cairo_set_source_rgba(cr, colour[0], colour[1], colour[2], 0.95 * alpha);
        cairo_set_line_width(cr, border);
        cairo_arc(cr, cx, cy, r, 0, 2 * std::numbers::pi);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
}

void ManaCoresSelector::draw(GtkDrawingArea* area G_GNUC_UNUSED, cairo_t* cr, int width G_GNUC_UNUSED, int height G_GNUC_UNUSED) {
    if (!visible_) return;

    // Set transparent background - ensure nothing opaque is left from previous frames
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Draw based on state
    switch (state_) {
    case State::Assembling:
        draw_core_animated(cr, radial_progress_[0], core_current_radius_, layout_.core_centre_x, layout_.core_centre_y);
        draw_radials_animated(cr, radial_progress_[0]);
        break;
    case State::Idle:
        draw_core(cr, 1.0);
        draw_radials(cr, 1.0);
        break;
    case State::Applying: {
        // Draw the apply animation with radial bloom effect
        double alpha = 1.0 - apply_progress_ * 0.5;  // Fade out slightly
        draw_core_animated(cr, alpha, core_current_radius_, layout_.core_centre_x, layout_.core_centre_y);
        draw_radials_animated(cr, alpha);
        
        // Draw Phase 10 FX during apply: edge glow, motes, and joining flash
        draw_phase10_edge_glow(cr);
        draw_phase10_motes(cr);
        
        // Joining flash at core center (mid-apply)
        if (apply_progress_ > 0.3 && apply_progress_ < 0.7) {
            draw_joining_flash(cr, layout_.core_centre_x, layout_.core_centre_y, apply_progress_);
        }
        break;
    }
    case State::Dismissing: {
        double alpha = radial_progress_[0];  // Fade out
        if (alpha > 0.0) {
            draw_core_animated(cr, alpha, core_current_radius_, layout_.core_centre_x, layout_.core_centre_y);
            draw_radials_animated(cr, alpha);
        }
        break;
    }
    default:
        break;
    }
}

void ManaCoresSelector::draw_callback(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer user_data) {
    auto* self = static_cast<ManaCoresSelector*>(user_data);
    self->draw(area, cr, width, height);
}

gboolean ManaCoresSelector::tick_callback(GtkWidget* widget, GdkFrameClock* frame_clock, gpointer user_data) {
    (void)widget;
    (void)frame_clock;
    auto* self = static_cast<ManaCoresSelector*>(user_data);
    if (!self->visible_) return G_SOURCE_REMOVE;

    guint64 now = g_get_monotonic_time();
    self->update_animations(now);
    return G_SOURCE_CONTINUE;
}

void ManaCoresSelector::update_animations(guint64 now_micros) {
    if (state_ == State::Assembling) {
        const double elapsed = static_cast<double>(now_micros - animation_start_micros_) / 1'000'000.0;  // seconds

        switch (assemble_phase_) {
        case AssemblePhase::CoreIn: {
            // Core fades and scales in over 250ms
            constexpr double kCoreInDuration = 0.25;
            double progress = std::min(elapsed / kCoreInDuration, 1.0);
            // Quadratic ease-out: t * (2 - t)
            progress = progress * (2.0 - progress);

            core_current_radius_ = layout_.core_radius * progress;
            radial_progress_[0] = progress;  // Use first radial's progress as alpha for core

            if (progress >= 1.0) {
                assemble_phase_ = AssemblePhase::FanIn;
                animation_start_micros_ = now_micros;
                // Reset radial progress for fan-in
                radial_progress_[0] = radial_progress_[1] = radial_progress_[2] = 0.0;
            }
            break;
        }

        case AssemblePhase::FanIn: {
            // Radials fan in sequentially: each takes 200ms, starts 80ms after previous
            constexpr double kFanInDuration = 0.20;
            constexpr double kFanInStagger = 0.08;

            for (int i = 0; i < 3; ++i) {
                double start_delay = i * kFanInStagger;
                double radial_elapsed = elapsed - start_delay;
                if (radial_elapsed <= 0) {
                    radial_progress_[i] = 0.0;
                } else {
                    double progress = std::min(radial_elapsed / kFanInDuration, 1.0);
                    // Quadratic ease-out
                    progress = progress * (2.0 - progress);
                    radial_progress_[i] = progress;
                }
            }

            // Check if all radials are done (last one + its duration + stagger)
            double total_fan_time = kFanInDuration + 2 * kFanInStagger;
            if (elapsed >= total_fan_time) {
                assemble_phase_ = AssemblePhase::Detach;
                animation_start_micros_ = now_micros;
                // Set up detach targets
                for (int i = 0; i < 3; ++i) {
                    radial_target_x_[i] = layout_.radials_park_x;
                    radial_target_y_[i] = layout_.radials_park_y + (i - 1) * layout_.radial_spacing;
                }
            }
            break;
        }

        case AssemblePhase::Detach: {
            // Radials slide from fan positions to parked stack, core expands to full radius
            constexpr double kDetachDuration = 0.25;
            double progress = std::min(elapsed / kDetachDuration, 1.0);
            progress = progress * (2.0 - progress);

            // Interpolate radial positions
            const double cx = layout_.core_centre_x;
            const double cy = layout_.core_centre_y;
            const double fan_r = layout_.fan_arc_radius;
            const double start_angle = layout_.fan_start_angle;
            const double angle_step = (layout_.fan_end_angle - layout_.fan_start_angle) / 3.0;

            for (int i = 0; i < 3; ++i) {
                double fan_angle = start_angle + i * angle_step;
                double fan_x = cx + fan_r * std::cos(fan_angle);
                double fan_y = cy + fan_r * std::sin(fan_angle);
                double park_x = layout_.radials_park_x;
                double park_y = layout_.radials_park_y + (i - 1) * layout_.radial_spacing;

                radial_target_x_[i] = fan_x + (park_x - fan_x) * progress;
                radial_target_y_[i] = fan_y + (park_y - fan_y) * progress;
            }

            // Core expands to full radius
            core_current_radius_ = layout_.core_radius;

            if (progress >= 1.0) {
                state_ = State::Idle;
                idle_start_micros_ = now_micros;
                // Initialize idle floating
                start_idle_animation();
            }
            break;
        }
        }

        queue_redraw();
    } else if (state_ == State::Applying) {
        const double elapsed = static_cast<double>(now_micros - apply_start_micros_) / 1'000'000.0;
        constexpr double kApplyDuration = 0.4;  // 400ms apply animation

        apply_progress_ = std::min(elapsed / kApplyDuration, 1.0);
        // Quadratic ease-out for the bloom
        // double bloom_progress = apply_progress_ * (2.0 - apply_progress_);  // Available if needed for shader

        // Core shrinks
        core_current_radius_ = layout_.core_radius * (1.0 - apply_progress_ * 0.65);  // Shrink to ~35%

        // Radials expand and surround the core
        const double cx = layout_.core_centre_x;
        const double cy = layout_.core_centre_y;
        const double surround_dist = layout_.core_radius_shrunk + 30.0;  // Distance from core centre

        for (int i = 0; i < 3; ++i) {
            double angle = (i * 2.0 * std::numbers::pi / 3.0) - std::numbers::pi / 2.0;  // Start at top, 120° apart
            double target_x = cx + surround_dist * std::cos(angle);
            double target_y = cy + surround_dist * std::sin(angle);

            // Interpolate from parked position to surround position
            double park_x = layout_.radials_park_x;
            double park_y = layout_.radials_park_y + (i - 1) * layout_.radial_spacing;
            radial_target_x_[i] = park_x + (target_x - park_x) * apply_progress_;
            radial_target_y_[i] = park_y + (target_y - park_y) * apply_progress_;
        }

        if (apply_progress_ >= 1.0) {
            // Apply complete - fire apply callback and start dismiss
            std::string selected_path;
            if (current_wallpaper_index_ >= 0 && current_wallpaper_index_ < static_cast<int>(next_wallpaper_paths_.size())) {
                selected_path = next_wallpaper_paths_[(current_wallpaper_index_ + 1) % 3];
            }
            if (apply_callback_) {
                apply_callback_(selected_path);
            }
            state_ = State::Dismissing;
            apply_start_micros_ = now_micros;
            apply_progress_ = 1.0;
        }

        queue_redraw();
    } else if (state_ == State::Dismissing) {
        const double elapsed = static_cast<double>(now_micros - apply_start_micros_) / 1'000'000.0;
        constexpr double kDismissDuration = 0.4;  // 400ms dismiss (Phase 9: exit transition)

        double progress = std::min(elapsed / kDismissDuration, 1.0);
        // Quadratic ease-in-out for smooth transition
        progress = progress < 0.5 ? 2.0 * progress * progress : 1.0 - 2.0 * (1.0 - progress) * (1.0 - progress);

        // Phase 9: Exit transition
        // 0-0.5: Radials fly outward, core shrinks
        // 0.5-1.0: Everything fades out
        
        if (progress < 0.5) {
            // First half: radials expand outward, core shrinks
            double phase_progress = progress * 2.0;
            
            // Core shrinks to near zero
            core_current_radius_ = layout_.core_radius * (1.0 - phase_progress * 0.95);
            
            // Radials fly outward to surround the core
            const double cx = layout_.core_centre_x;
            const double cy = layout_.core_centre_y;
            const double fly_distance = 200.0;  // Distance to fly out
            
            for (int i = 0; i < 3; ++i) {
                double angle = (i * 2.0 * std::numbers::pi / 3.0) - std::numbers::pi / 2.0;
                double start_x = layout_.radials_park_x;
                double start_y = layout_.radials_park_y + (i - 1) * layout_.radial_spacing;
                double end_x = cx + fly_distance * std::cos(angle);
                double end_y = cy + fly_distance * std::sin(angle);
                
                radial_target_x_[i] = start_x + (end_x - start_x) * phase_progress;
                radial_target_y_[i] = start_y + (end_y - start_y) * phase_progress;
            }
        } else {
            // Second half: everything fades out
            double fade_progress = (progress - 0.5) * 2.0;
            double alpha = 1.0 - fade_progress;
            radial_progress_[0] = alpha;  // Use as alpha for fade
            
            // Continue flying out slightly
            if (fade_progress < 1.0) {
                const double cx = layout_.core_centre_x;
                const double cy = layout_.core_centre_y;
                
                for (int i = 0; i < 3; ++i) {
                    double angle = (i * 2.0 * std::numbers::pi / 3.0) - std::numbers::pi / 2.0;
                    double current_x = radial_target_x_[i];
                    double current_y = radial_target_y_[i];
                    double end_x = cx + 250.0 * std::cos(angle);
                    double end_y = cy + 250.0 * std::sin(angle);
                    
                    radial_target_x_[i] = current_x + (end_x - current_x) * fade_progress;
                    radial_target_y_[i] = current_y + (end_y - current_y) * fade_progress;
                }
            }
        }

        if (progress >= 1.0) {
            state_ = State::Hidden;
            dismiss();
        }

        queue_redraw();
    }
}

void ManaCoresSelector::start_idle_animation() {
    // Initialize seeded floating parameters for each radial
    // Using fixed seeds for deterministic but varied motion
    static constexpr uint32_t seeds[3] = {0x9E3779B9, 0x243F6A88, 0x13198A2E};

    for (int i = 0; i < 3; ++i) {
        uint32_t seed = seeds[i];

        // X period: 3.0 - 5.0 seconds
        seed = seed * 1664525 + 1013904223;
        idle_float_params_[i].period_x = 3.0 + (seed % 1000) / 1000.0 * 2.0;

        // Y period: 2.5 - 4.5 seconds
        seed = seed * 1664525 + 1013904223;
        idle_float_params_[i].period_y = 2.5 + (seed % 1000) / 1000.0 * 2.0;

        // Phase offsets: 0 - 2π
        seed = seed * 1664525 + 1013904223;
        idle_float_params_[i].phase_x = (seed % 1000) / 1000.0 * 2.0 * std::numbers::pi;

        seed = seed * 1664525 + 1013904223;
        idle_float_params_[i].phase_y = (seed % 1000) / 1000.0 * 2.0 * std::numbers::pi;

        // Amplitude: 2-4px scaled
        seed = seed * 1664525 + 1013904223;
        double scale = layout_.canvas_height / 1080.0;
        idle_float_params_[i].amplitude_x = (2.0 + (seed % 1000) / 1000.0 * 2.0) * scale;

        seed = seed * 1664525 + 1013904223;
        idle_float_params_[i].amplitude_y = (2.0 + (seed % 1000) / 1000.0 * 2.0) * scale;
    }
}

void ManaCoresSelector::request_apply() {
    if (state_ != State::Idle) return;

    begin_apply();
}

void ManaCoresSelector::begin_apply() {
    state_ = State::Applying;
    apply_start_micros_ = g_get_monotonic_time();
    apply_progress_ = 0.0;

    // Initialize bloom shader if not already done
    if (!bloom_shader_) {
        bloom_shader_ = std::make_unique<RadialBloomShader>();
        if (!bloom_shader_->compile()) {
            std::cerr << "[ManaCoresSelector] Failed to compile bloom shader, falling back to Cairo\n";
            bloom_shader_.reset();
        }
    }

    // Core starts at full radius and will shrink
    core_current_radius_ = layout_.core_radius;

    // Radials start at parked positions
    for (int i = 0; i < 3; ++i) {
        radial_target_x_[i] = layout_.radials_park_x;
        radial_target_y_[i] = layout_.radials_park_y + (i - 1) * layout_.radial_spacing;
    }

    queue_redraw();
}

void ManaCoresSelector::draw_joining_flash(cairo_t* cr, double cx, double cy, double progress) {
    // Joining flash at core center - bright white flash expanding outward
    double flash_progress = (progress - 0.3) / 0.4;  // Normalize to 0..1 between 0.3 and 0.7
    if (flash_progress <= 0.0 || flash_progress >= 1.0) return;
    
    // Quadratic ease-out for flash intensity
    double intensity = 1.0 - flash_progress * (2.0 - flash_progress);
    double max_radius = layout_.core_radius * 2.0;
    double flash_radius = max_radius * flash_progress;
    
    cairo_save(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, intensity * 0.8);
    cairo_set_line_width(cr, 3.0);
    cairo_arc(cr, cx, cy, flash_radius, 0, 2 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_restore(cr);
    
    // Inner glow
    cairo_save(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, intensity * 0.4);
    cairo_arc(cr, cx, cy, flash_radius * 0.5, 0, 2 * std::numbers::pi);
    cairo_fill(cr);
    cairo_restore(cr);
}

void ManaCoresSelector::draw_phase10_edge_glow(cairo_t* cr) {
    // Edge glow around the core portal - only during Applying phase
    const double cx = layout_.core_centre_x;
    const double cy = layout_.core_centre_y;
    const double r = layout_.core_radius;
    const double glow = layout_.glow_extent;
    
    if (state_ != State::Applying) return;
    
    // Glow pulses during apply animation
    double pulse = 0.5 + 0.5 * std::sin(apply_progress_ * 4.0 * 3.14159);
    
    cairo_save(cr);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.3 * pulse);
    cairo_set_line_width(cr, glow * pulse);
    cairo_arc(cr, cx, cy, r, 0, 2 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void ManaCoresSelector::draw_phase10_motes(cairo_t* cr) {
    // Floating motes/particles around the core - only during Applying phase
    if (state_ != State::Applying) return;
    
    const double cx = layout_.core_centre_x;
    const double cy = layout_.core_centre_y;
    const double r = layout_.core_radius;
    
    double t = apply_progress_;
    
    // Draw 8 motes orbiting around the core
    for (int i = 0; i < 8; ++i) {
        double angle = (i * 2.0 * std::numbers::pi / 8.0) + t * 3.0;  // Rotation during apply
        double orbit_r = r + 30.0 + 10.0 * std::sin(t * 1.2 + i * 0.5);
        double mx = cx + orbit_r * std::cos(angle);
        double my = cy + orbit_r * std::sin(angle);
        double mote_size = 2.0 + 1.0 * std::sin(t * 3.0 + i);
        double alpha = (0.3 + 0.4 * std::sin(t * 2.0 + i * 0.8)) * (1.0 - t);  // Fade out as apply completes
        
        cairo_save(cr);
        cairo_set_source_rgba(cr, 1.0, 0.9, 0.7, alpha);
        cairo_arc(cr, mx, my, mote_size, 0, 2 * std::numbers::pi);
        cairo_fill(cr);
        cairo_restore(cr);
    }
}

[[nodiscard]] bool ManaCoresSelector::handle_key(guint keyval) {
    if (state_ == State::Idle || state_ == State::Assembling) {
        switch (keyval) {
        case GDK_KEY_Escape:
        case GDK_KEY_Q:
            dismiss();
            return true;
        case GDK_KEY_Up:
        case GDK_KEY_k:
            if (hovered_radial_ > 0) {
                hovered_radial_--;
                queue_redraw();
                return true;
            }
            break;
        case GDK_KEY_Down:
        case GDK_KEY_j:
            if (hovered_radial_ < 2) {
                hovered_radial_++;
                queue_redraw();
                return true;
            }
            break;
        case GDK_KEY_Left:
        case GDK_KEY_h:
            // Cycle wallpaper selection left (previous)
            cycle_wallpaper(-1);
            return true;
        case GDK_KEY_Right:
        case GDK_KEY_l:
            // Cycle wallpaper selection right (next)
            cycle_wallpaper(1);
            return true;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
            request_apply();
            return true;
        default:
            break;
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