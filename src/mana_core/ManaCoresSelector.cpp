#include "mana_core/ManaCoresSelector.hpp"

#include <cstdlib>
#include <cmath>
#include <numbers>
#include <memory>
#include <iostream>
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cairo.h>
#include "effects/core/ShaderSource.hpp"
#include "mana_core/ThumbnailCache.hpp"
#include "ui/LayerSurface.hpp"

namespace realmheart::mana_core {
namespace {

constexpr std::string_view kVertexShader = R"GLSL(#version 300 es
precision highp float;

out vec2 v_texcoord;

void main() {
    vec2 corner = vec2(
        float((gl_VertexID << 1) & 2),
        float(gl_VertexID & 2)
    );
    v_texcoord = vec2(corner.x, 1.0 - corner.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

constexpr std::string_view kDefaultManaCoreSmokeFragment = R"GLSL(// White Core Smoke — TBATE Lore-Accurate Mana Core Ethereal Mist
// Billowing pure white and silver smoke orbiting the White Mana Core.
// Smoothly wraps the core boundary with organic FBM turbulence and micro-motes,
// strictly masking the inner circle to preserve full wallpaper clarity.
// SPDX-License-Identifier: GPL-3.0-or-later

#version 300 es
precision highp float;

in vec2 v_texcoord;

uniform float u_time;
uniform vec2  u_resolution;
uniform vec2  u_core_center;
uniform float u_core_radius;
uniform float u_alpha;
uniform float u_heartbeat;

layout(location = 0) out vec4 fragColor;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x),
        mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x),
        u.y
    );
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.55;
    for (int i = 0; i < 3; i++) {
        v += noise2(p) * a;
        p = p * 2.15 + 13.37;
        a *= 0.5;
    }
    return v;
}

void main() {
    if (u_alpha <= 0.001 || u_core_radius <= 2.0) {
        fragColor = vec4(0.0);
        return;
    }

    vec2 pixelPos = v_texcoord * u_resolution;
    vec2 d = pixelPos - u_core_center;
    float r = length(d);

    // Completely transparent inside the inner core circle to ensure zero wallpaper distortion
    if (r < u_core_radius - 2.0) {
        fragColor = vec4(0.0);
        return;
    }

    float scale = u_resolution.y / 1080.0;
    float angle = (r > 0.0) ? atan(d.y, d.x) : 0.0;

    // Smooth transition right at the core rim
    float inner_mask = smoothstep(u_core_radius - 1.0, u_core_radius + 4.0 * scale, r);

    // Multi-octave organic turbulence at the core boundary
    vec2 polar_uv = vec2(cos(angle) * 3.2 + u_time * 0.22, sin(angle) * 3.2 - u_time * 0.18);
    float boundary_turb = (fbm(polar_uv) - 0.5) * (14.0 * scale);
    float distorted_edge = u_core_radius + boundary_turb;

    // Radial smoke envelope hugging the core (static — no heartbeat flicker)
    float dEdge = r - distorted_edge;
    float band_width = 48.0 * scale;
    float env = exp(-pow(max(0.0, dEdge - band_width * 0.15) / (band_width * 0.45), 2.0));

    // Orbital swirling smoke (volumetric billows rolling along the rim)
    vec2 swirl_coord = vec2(angle * 2.2 + u_time * 0.30, (r - u_core_radius) * 0.035 - u_time * 0.15);
    float dens1 = fbm(swirl_coord * 2.5 + vec2(3.1, 7.8));
    float dens2 = fbm(vec2(d.x * 0.018 + u_time * 0.12, d.y * 0.018 - u_time * 0.10));
    float smoke = clamp(env * (dens1 * 1.5 + dens2 * 0.8), 0.0, 1.0);

    // Ethereal wisps trailing outwards
    vec2 wisp_coord = vec2(angle * 4.5 - u_time * 0.45, (r - u_core_radius) * 0.022);
    float wisps = pow(fbm(wisp_coord * 2.8), 2.0) * exp(-max(0.0, r - u_core_radius) / (band_width * 1.5));

    // Radiant inner white rim glow (static — no heartbeat flicker)
    float rim_glow = exp(-max(0.0, r - u_core_radius) / (12.0 * scale)) * 0.85;

    // Luminous micro-motes of pure mana glittering in the smoke
    vec2 mote_uv = pixelPos / (26.0 * scale);
    vec2 mote_id = floor(mote_uv);
    float mote_hash = hash12(mote_id + 5.31);
    vec2 mote_subpos = mote_id + 0.2 + 0.6 * vec2(hash12(mote_id + 1.7), hash12(mote_id + 9.3));
    float mote_d2 = dot(mote_uv - mote_subpos, mote_uv - mote_subpos);
    float twinkle = 0.5 + 0.5 * sin(u_time * 5.0 + mote_hash * 30.0);
    float mote = step(0.76, mote_hash) * exp(-mote_d2 * 55.0) * smoke * twinkle * 0.8;

    // Pure White / Silver / Grey palette (TBATE Lore-Accurate White Core)
    vec3 col_pure_white = vec3(1.0, 1.0, 1.0);
    vec3 col_silver = vec3(0.86, 0.90, 0.95);
    vec3 col_slate = vec3(0.65, 0.70, 0.76);

    // Blend colours from core white to outer silver/slate smoke
    vec3 smoke_color = mix(col_slate, col_silver, smoothstep(0.1, 0.6, smoke));
    smoke_color = mix(smoke_color, col_pure_white, rim_glow * 0.8 + mote * 1.0);

    // Total alpha computation
    float combined_alpha = clamp((smoke * 0.90 + wisps * 0.55 + rim_glow * 0.75 + mote * 1.2) * inner_mask * u_alpha, 0.0, 1.0);

    // Premultiplied alpha output for OpenGL compositing
    fragColor = vec4(smoke_color * combined_alpha, combined_alpha);
}
)GLSL";

GLuint compile_shader(GLenum type, std::string_view source, std::string* error) {
    const GLuint shader = glCreateShader(type);
    const char* source_pointer = source.data();
    const GLint source_length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &source_pointer, &source_length);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    glDeleteShader(shader);
    if (error != nullptr) *error = std::move(log);
    return 0;
}

GLuint link_program(std::string_view fragment_source, std::string* error) {
    std::string vertex_error;
    const GLuint vertex = compile_shader(GL_VERTEX_SHADER, kVertexShader, &vertex_error);
    if (vertex == 0) {
        if (error != nullptr) *error = "vertex shader failed: " + vertex_error;
        return 0;
    }

    std::string fragment_error;
    const GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source, &fragment_error);
    if (fragment == 0) {
        glDeleteShader(vertex);
        if (error != nullptr) *error = "fragment shader failed: " + fragment_error;
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) return program;

    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    glDeleteProgram(program);
    if (error != nullptr) *error = "shader link failed: " + log;
    return 0;
}

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
    cleanup_gl_resources();
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
    // Toggle: if already visible, dismiss instead of re-presenting
    if (visible_) {
        if (state_ == State::Assembling || state_ == State::Idle) {
            begin_dismiss();
        }
        return;
    }
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
    particles_.fill(ManaParticle{});
    last_tick_micros_ = 0;

    setup_window(app);

    if (tick_callback_id_ == 0) {
        tick_callback_id_ = gtk_widget_add_tick_callback(canvas_, tick_callback, this, nullptr);
    }

    if (gl_area_) {
        gtk_widget_set_visible(gl_area_, TRUE);
        gtk_gl_area_queue_render(GTK_GL_AREA(gl_area_));
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
    particles_.fill(ManaParticle{});
    last_tick_micros_ = 0;
    if (tick_callback_id_ != 0 && canvas_ != nullptr) {
        gtk_widget_remove_tick_callback(canvas_, tick_callback_id_);
        tick_callback_id_ = 0;
    }
    clear_old_pixbufs();
    if (apply_fullscreen_pixbuf_ != nullptr) {
        g_object_unref(apply_fullscreen_pixbuf_);
        apply_fullscreen_pixbuf_ = nullptr;
    }
    if (gl_area_) {
        gtk_widget_set_visible(gl_area_, FALSE);
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

    // GL smoke renders as the base layer of the overlay.
    gl_area_ = gtk_gl_area_new();
    gtk_gl_area_set_allowed_apis(
        GTK_GL_AREA(gl_area_),
        GDK_GL_API_GLES
    );
    gtk_gl_area_set_required_version(GTK_GL_AREA(gl_area_), 3, 0);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(gl_area_), FALSE);
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(gl_area_), FALSE);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(gl_area_), TRUE);
    gtk_widget_set_hexpand(gl_area_, TRUE);
    gtk_widget_set_vexpand(gl_area_, TRUE);
    gtk_widget_set_visible(gl_area_, TRUE);

    g_signal_connect(gl_area_, "render", G_CALLBACK(gl_render_callback), this);
    g_signal_connect(gl_area_, "unrealize", G_CALLBACK(gl_unrealize_callback), this);

    // GL smoke renders as the base layer; Cairo canvas overlays UI on top.
    gtk_overlay_set_child(GTK_OVERLAY(root), GTK_WIDGET(gl_area_));

    // Top layer: GtkDrawingArea for crisp UI borders, slices, runes, wallpaper preview, and motes
    canvas_ = gtk_drawing_area_new();
    gtk_widget_add_css_class(GTK_WIDGET(canvas_), "realmheart-mana-cores-canvas");
    gtk_widget_remove_css_class(GTK_WIDGET(canvas_), "background");
    gtk_widget_set_visible(canvas_, TRUE);
    gtk_widget_set_hexpand(canvas_, TRUE);
    gtk_widget_set_vexpand(canvas_, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(canvas_), draw_callback, this, nullptr);
    gtk_overlay_add_overlay(GTK_OVERLAY(root), GTK_WIDGET(canvas_));
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

void ManaCoresSelector::draw_realmheart_runes(
    cairo_t* cr,
    double cx, double cy,
    double radius,
    double alpha
) {
    if (alpha <= 0.02 || radius <= 20.0) return;

    cairo_save(cr);

    double t = static_cast<double>(g_get_monotonic_time()) / 1'000'000.0;
    double scale = layout_.canvas_height / 1080.0;

    // 1. Concentric orbital rune rings in the orbit gap
    double ring1_r = radius + 8.0 * scale;
    double ring2_r = radius + 15.0 * scale;

    // Inner dashed Aether ring (slow clockwise rotation)
    cairo_save(cr);
    cairo_arc(cr, cx, cy, ring1_r, 0, 2.0 * std::numbers::pi);
    const double dashes1[] = {5.0 * scale, 9.0 * scale};
    cairo_set_dash(cr, dashes1, 2, t * 14.0);
    cairo_set_line_width(cr, 1.2 * scale);
    // Subtle Aether violet / lavender tone
    cairo_set_source_rgba(cr, 0.78, 0.62, 0.98, 0.28 * alpha);
    cairo_stroke(cr);
    cairo_restore(cr);

    // Outer dotted Mana ring (counter-clockwise rotation)
    cairo_save(cr);
    cairo_arc(cr, cx, cy, ring2_r, 0, 2.0 * std::numbers::pi);
    const double dashes2[] = {2.0 * scale, 6.0 * scale};
    cairo_set_dash(cr, dashes2, 2, -t * 10.0);
    cairo_set_line_width(cr, 0.9 * scale);
    // Crisp mana cyan-white tone
    cairo_set_source_rgba(cr, 0.85, 0.92, 1.0, 0.22 * alpha);
    cairo_stroke(cr);
    cairo_restore(cr);

    // 2. 8 Geometric God Step / Realmheart tick marks at 45° intervals
    for (int k = 0; k < 8; ++k) {
        double theta = k * (std::numbers::pi * 0.25) + t * 0.04;
        double cos_t = std::cos(theta);
        double sin_t = std::sin(theta);

        double tick_r_in = radius + 5.0 * scale;
        double tick_r_out = radius + 18.0 * scale;

        cairo_move_to(cr, cx + tick_r_in * cos_t, cy + tick_r_in * sin_t);
        cairo_line_to(cr, cx + tick_r_out * cos_t, cy + tick_r_out * sin_t);
        cairo_set_source_rgba(cr, 0.88, 0.82, 1.0, 0.32 * alpha);
        cairo_set_line_width(cr, (k % 2 == 0) ? (1.5 * scale) : (0.8 * scale));
        cairo_stroke(cr);

        if (k % 2 == 0) {
            double dx = cx + (tick_r_out + 3.0 * scale) * cos_t;
            double dy = cy + (tick_r_out + 3.0 * scale) * sin_t;
            cairo_arc(cr, dx, dy, 1.6 * scale, 0, 2.0 * std::numbers::pi);
            cairo_set_source_rgba(cr, 0.95, 0.90, 1.0, 0.50 * alpha);
            cairo_fill(cr);
        }
    }

    cairo_restore(cr);
}

void ManaCoresSelector::draw_drop_shadows(
    cairo_t* cr,
    double cx, double cy,
    double core_radius,
    double r_in, double r_out,
    double alpha
) {
    if (alpha <= 0.01) return;

    cairo_save(cr);
    double scale = layout_.canvas_height / 1080.0;

    // 1. Core Drop Shadow (elevates the main disk)
    if (core_radius > 10.0) {
        const double shadow_dx = 8.0 * scale;
        const double shadow_dy = 12.0 * scale;

        // Soft outer ambient shadow
        cairo_arc(cr, cx + shadow_dx, cy + shadow_dy, core_radius + 4.0, 0, 2.0 * std::numbers::pi);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.04, 0.40 * alpha);
        cairo_set_line_width(cr, 28.0 * scale);
        cairo_stroke(cr);

        // Tighter contact shadow
        cairo_arc(cr, cx + shadow_dx * 0.6, cy + shadow_dy * 0.6, core_radius, 0, 2.0 * std::numbers::pi);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.02, 0.60 * alpha);
        cairo_set_line_width(cr, 10.0 * scale);
        cairo_stroke(cr);
    }

    // 2. Radial Slices Drop Shadows
    if (r_in > 0.0 && r_out > r_in) {
        for (size_t i = 0; i < 3; ++i) {
            const auto& geom = current_slices_[i];
            double slice_cx = cx;
            double slice_cy = cy;
            double slice_r_in = r_in;
            double slice_r_out = r_out;

            if (state_ == State::Idle && hovered_radial_ == static_cast<int>(i)) {
                double pop = 14.0 * scale;
                slice_cx += pop * std::cos(geom.mid_angle);
                slice_cy += pop * std::sin(geom.mid_angle);
                slice_r_out += 8.0 * scale;
            }

            const double shadow_dx = 6.0 * scale;
            const double shadow_dy = 10.0 * scale;

            // Ambient slice shadow
            append_annular_sector_path(
                cr, slice_cx + shadow_dx, slice_cy + shadow_dy,
                slice_r_in, slice_r_out,
                geom.start_angle, geom.end_angle
            );
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.03, 0.35 * alpha);
            cairo_set_line_width(cr, (layout_.border_thickness + 16.0) * scale);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_stroke(cr);

            // Contact slice shadow
            append_annular_sector_path(
                cr, slice_cx + shadow_dx * 0.6, slice_cy + shadow_dy * 0.6,
                slice_r_in, slice_r_out,
                geom.start_angle, geom.end_angle
            );
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.02, 0.55 * alpha);
            cairo_set_line_width(cr, (layout_.border_thickness + 6.0) * scale);
            cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
            cairo_stroke(cr);
        }
    }

    cairo_restore(cr);
}

bool ManaCoresSelector::ensure_gl_program() {
    if (gl_program_ != 0) return true;

    std::string frag_src;
    std::string error;
    if (auto src = realmheart::effects::load_shader_source("mana-core/smoke/smoke.frag", &error); src.has_value()) {
        frag_src = src->text;
    } else {
        frag_src = kDefaultManaCoreSmokeFragment;
    }

    gl_program_ = link_program(frag_src, &error);
    if (gl_program_ == 0) {
        std::cerr << "[ManaCoresSelector] GLSL link failed: " << error << '\n';
        return false;
    }

    glGenVertexArrays(1, &gl_vao_);
    return true;
}

void ManaCoresSelector::cleanup_gl_resources() noexcept {
    if (gl_area_ != nullptr && gtk_widget_get_realized(gl_area_)) {
        gtk_gl_area_make_current(GTK_GL_AREA(gl_area_));
        if (gtk_gl_area_get_error(GTK_GL_AREA(gl_area_)) == nullptr) {
            if (gl_vao_ != 0) glDeleteVertexArrays(1, &gl_vao_);
            if (gl_program_ != 0) glDeleteProgram(gl_program_);
        }
    }
    gl_vao_ = 0;
    gl_program_ = 0;
}

gboolean ManaCoresSelector::gl_render_callback(GtkGLArea* area, GdkGLContext* context, gpointer user_data) {
    auto* self = static_cast<ManaCoresSelector*>(user_data);
    if (self == nullptr) return TRUE;
    return self->render_gl(area, context);
}

void ManaCoresSelector::gl_unrealize_callback(GtkWidget* widget, gpointer user_data) {
    (void)widget;
    auto* self = static_cast<ManaCoresSelector*>(user_data);
    if (self != nullptr) {
        self->cleanup_gl_resources();
    }
}

gboolean ManaCoresSelector::render_gl(GtkGLArea* area, GdkGLContext*) noexcept {
    if (!visible_ || current_alpha_ <= 0.001 || current_core_radius_ <= 2.0) {
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        return TRUE;
    }

    if (const GError* gl_error = gtk_gl_area_get_error(area); gl_error != nullptr) {
        return TRUE;
    }

    if (!ensure_gl_program()) {
        return TRUE;
    }

    const int scale = std::max(gtk_widget_get_scale_factor(GTK_WIDGET(area)), 1);
    const int width = std::max(gtk_widget_get_width(GTK_WIDGET(area)) * scale, 1);
    const int height = std::max(gtk_widget_get_height(GTK_WIDGET(area)) * scale, 1);

    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(gl_program_);
    glBindVertexArray(gl_vao_);

    double t = static_cast<double>(g_get_monotonic_time()) / 1'000'000.0;
    double beat_time = std::fmod(t, 1.35);
    double heartbeat = 0.0;
    if (beat_time < 0.16) {
        heartbeat = std::sin((beat_time / 0.16) * std::numbers::pi);
    } else if (beat_time >= 0.22 && beat_time < 0.38) {
        heartbeat = 0.50 * std::sin(((beat_time - 0.22) / 0.16) * std::numbers::pi);
    }

    glUniform1f(glGetUniformLocation(gl_program_, "u_time"), static_cast<float>(t));
    glUniform2f(glGetUniformLocation(gl_program_, "u_resolution"), static_cast<float>(width), static_cast<float>(height));
    glUniform2f(
        glGetUniformLocation(gl_program_, "u_core_center"),
        static_cast<float>(current_cx_ * scale),
        static_cast<float>(current_cy_ * scale)
    );
    glUniform1f(
        glGetUniformLocation(gl_program_, "u_core_radius"),
        static_cast<float>(current_core_radius_ * scale)
    );
    glUniform1f(
        glGetUniformLocation(gl_program_, "u_alpha"),
        static_cast<float>(current_alpha_ * current_wallpaper_alpha_)
    );
    glUniform1f(
        glGetUniformLocation(gl_program_, "u_heartbeat"),
        static_cast<float>(heartbeat)
    );

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glUseProgram(0);

    return TRUE;
}

void ManaCoresSelector::draw_core(
    cairo_t* cr,
    double cx, double cy,
    double radius,
    double alpha,
    double wallpaper_alpha
) {
    if (radius <= 0.0 || alpha <= 0.0) return;

    double t = static_cast<double>(g_get_monotonic_time()) / 1'000'000.0;
    double scale = layout_.canvas_height / 1080.0;

    // 1. Wallpaper inside core with 2.5D Lissajous floating parallax
    if (wallpaper_alpha > 0.0) {
        cairo_save(cr);
        cairo_arc(cr, cx, cy, radius, 0, 2.0 * std::numbers::pi);
        cairo_clip(cr);

        double parallax_x = (7.0 * std::sin(t * 0.65) + 3.0 * std::cos(t * 1.3)) * scale;
        double parallax_y = (5.0 * std::cos(t * 0.85) + 2.0 * std::sin(t * 1.7)) * scale;

        double dir = static_cast<double>(nav_direction_);
        double box = radius * 2.4;

        if (nav_progress_ < 1.0 && old_core_pixbuf_ != nullptr) {
            double travel = radius * 0.6;
            double old_progress = nav_progress_;
            double old_dx = -travel * std::cos(42.0 * std::numbers::pi / 180.0) * old_progress * dir;
            double old_dy = travel * std::sin(42.0 * std::numbers::pi / 180.0) * old_progress * dir;
            double old_rot = -0.35 * old_progress * dir;

            cairo_save(cr);
            cairo_translate(cr, cx + old_dx + parallax_x, cy + old_dy + parallax_y);
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
            double ease = (nav_progress_ < 1.0) ? (1.0 - std::pow(1.0 - nav_progress_, 3.0)) : 1.0;
            double travel = radius * 0.75;
            double in_dx = travel * std::cos(-42.0 * std::numbers::pi / 180.0) * (1.0 - ease) * dir;
            double in_dy = travel * std::sin(-42.0 * std::numbers::pi / 180.0) * (1.0 - ease) * dir;
            double in_rot = 0.45 * (1.0 - ease) * dir;

            cairo_save(cr);
            cairo_translate(cr, cx + in_dx + parallax_x, cy + in_dy + parallax_y);
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

        // Subtle dark rim vignette with Aether violet depth
        cairo_pattern_t* vignette = cairo_pattern_create_radial(
            cx, cy, radius * 0.65,
            cx, cy, radius
        );
        cairo_pattern_add_color_stop_rgba(vignette, 0.0, 0.0, 0.0, 0.0, 0.0);
        cairo_pattern_add_color_stop_rgba(vignette, 0.72, 0.35, 0.12, 0.55, 0.22 * alpha); // Aether violet hint
        cairo_pattern_add_color_stop_rgba(vignette, 1.0, 0.0, 0.0, 0.02, 0.45 * alpha);
        cairo_set_source(cr, vignette);
        cairo_paint(cr);
        cairo_pattern_destroy(vignette);

        cairo_restore(cr);
    }

    const double border = layout_.border_thickness;
    const double glow = layout_.glow_extent;

    // 2. White & Aether Mana Core Flowing Animated Gradients
    double grad_angle = t * 1.4;
    double gx1 = cx + radius * std::cos(grad_angle);
    double gy1 = cy + radius * std::sin(grad_angle);
    double gx2 = cx - radius * std::cos(grad_angle);
    double gy2 = cy - radius * std::sin(grad_angle);

    cairo_pattern_t* core_glow_pattern = cairo_pattern_create_linear(gx1, gy1, gx2, gy2);
    cairo_pattern_add_color_stop_rgba(core_glow_pattern, 0.0, 1.0, 1.0, 1.0, 0.45 * alpha);
    cairo_pattern_add_color_stop_rgba(core_glow_pattern, 0.35, 0.82, 0.94, 1.0, 0.40 * alpha);
    cairo_pattern_add_color_stop_rgba(core_glow_pattern, 0.70, 0.88, 0.72, 1.0, 0.42 * alpha); // Aether
    cairo_pattern_add_color_stop_rgba(core_glow_pattern, 1.0, 1.0, 1.0, 1.0, 0.45 * alpha);

    // Outer Glow (static — no heartbeat flicker)
    cairo_save(cr);
    cairo_set_source(cr, core_glow_pattern);
    cairo_set_line_width(cr, border + glow);
    cairo_arc(cr, cx, cy, radius, 0, 2.0 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_restore(cr);

    // Inner Glow (static — no heartbeat flicker)
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.95, 0.98, 1.0, 0.28 * alpha);
    cairo_set_line_width(cr, border + (glow * 0.5));
    cairo_arc(cr, cx, cy, radius, 0, 2.0 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_restore(cr);

    // Crisp Hard Border with animated gradient flow
    cairo_save(cr);
    cairo_pattern_t* core_border_pattern = cairo_pattern_create_linear(gx1, gy1, gx2, gy2);
    cairo_pattern_add_color_stop_rgba(core_border_pattern, 0.0, 1.0, 1.0, 1.0, 0.98 * alpha);
    cairo_pattern_add_color_stop_rgba(core_border_pattern, 0.5, 0.90, 0.95, 1.0, 0.95 * alpha);
    cairo_pattern_add_color_stop_rgba(core_border_pattern, 1.0, 1.0, 1.0, 1.0, 0.98 * alpha);
    cairo_set_source(cr, core_border_pattern);
    cairo_set_line_width(cr, border);
    cairo_arc(cr, cx, cy, radius, 0, 2.0 * std::numbers::pi);
    cairo_stroke(cr);
    cairo_pattern_destroy(core_border_pattern);
    cairo_pattern_destroy(core_glow_pattern);
    cairo_restore(cr);
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
    double t = static_cast<double>(g_get_monotonic_time()) / 1'000'000.0;
    double scale = layout_.canvas_height / 1080.0;

    for (size_t i = 0; i < 3; ++i) {
        const auto& geom = current_slices_[i];
        const auto& color = layout_.kRadialPalette[i];
        GdkPixbuf* pixbuf = slice_pixbufs_[i];

        double slice_cx = cx;
        double slice_cy = cy;
        double slice_r_in = r_in;
        double slice_r_out = r_out;
        double glow_boost = 1.0;

        // Hover animation with organic double-beat pulse for selected radial
        if (state_ == State::Idle && hovered_radial_ == static_cast<int>(i)) {
            double h_t = static_cast<double>(g_get_monotonic_time() - idle_start_micros_) / 1'000'000.0;
            double beat = std::fmod(h_t, 1.35);
            double hover_pulse = 0.0;
            if (beat < 0.16) {
                hover_pulse = std::sin((beat / 0.16) * std::numbers::pi);
            } else if (beat >= 0.22 && beat < 0.38) {
                hover_pulse = 0.50 * std::sin(((beat - 0.22) / 0.16) * std::numbers::pi);
            }
            glow_boost = 1.2 + 0.6 * hover_pulse;
            double pop = (14.0 + 3.0 * hover_pulse) * scale;
            slice_cx += pop * std::cos(geom.mid_angle);
            slice_cy += pop * std::sin(geom.mid_angle);
            slice_r_out += (8.0 + 2.0 * hover_pulse) * scale;
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

        // 1b. Wallpaper preview inside slice with 2.5D micro-parallax
        if (wallpaper_alpha > 0.0) {
            cairo_save(cr);
            append_annular_sector_path(
                cr, slice_cx, slice_cy,
                slice_r_in, slice_r_out,
                geom.start_angle, geom.end_angle
            );
            cairo_clip(cr);

            double mid_r = (slice_r_in + slice_r_out) * 0.5;
            double arc_span = std::abs(geom.end_angle - geom.start_angle);
            double arc_width = slice_r_out * arc_span;
            double radial_depth = slice_r_out - slice_r_in;
            
            double bb_w = arc_width * 1.15;
            double bb_h = radial_depth * 1.25;

            double immersion_tweak = 0.0;
            if (i == 0) immersion_tweak = -0.12;
            else if (i == 2) immersion_tweak = 0.12;

            double slice_plx = (4.0 * std::sin(t * 0.85 + i * 1.4)) * scale;
            double slice_ply = (3.0 * std::cos(t * 0.95 + i * 1.4)) * scale;

            double s_dir = static_cast<double>(nav_direction_);
            constexpr double delta_angle = 42.0 * std::numbers::pi / 180.0;

            if (nav_progress_ < 1.0 && old_slice_pixbufs_[i] != nullptr) {
                double old_angle_offset = -delta_angle * nav_progress_ * s_dir;
                double old_angle = geom.mid_angle + old_angle_offset;
                double old_x = slice_cx + mid_r * std::cos(old_angle);
                double old_y = slice_cy + mid_r * std::sin(old_angle);
                double old_rot = old_angle + (std::numbers::pi / 2.0) + immersion_tweak;

                cairo_save(cr);
                cairo_translate(cr, old_x + slice_plx, old_y + slice_ply);
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
                cairo_translate(cr, cur_x + slice_plx, cur_y + slice_ply);
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

        // 2. Coloured border & glow with Dynamic "Mana Flow" Gradients
        cairo_save(cr);

        double flow_t = t * 2.2 + i * 1.6;
        double gx1 = slice_cx + slice_r_out * std::cos(geom.start_angle + 0.12 * std::sin(flow_t));
        double gy1 = slice_cy + slice_r_out * std::sin(geom.start_angle + 0.12 * std::sin(flow_t));
        double gx2 = slice_cx + slice_r_out * std::cos(geom.end_angle + 0.12 * std::cos(flow_t));
        double gy2 = slice_cy + slice_r_out * std::sin(geom.end_angle + 0.12 * std::cos(flow_t));

        cairo_pattern_t* slice_grad = cairo_pattern_create_linear(gx1, gy1, gx2, gy2);
        double shift = 0.5 + 0.35 * std::sin(flow_t);
        cairo_pattern_add_color_stop_rgba(slice_grad, 0.0,
            std::min(1.0, color[0] + 0.15), std::min(1.0, color[1] + 0.15), std::min(1.0, color[2] + 0.15),
            0.85 * alpha);
        cairo_pattern_add_color_stop_rgba(slice_grad, shift,
            1.0, std::min(1.0, color[1] + 0.35), std::min(1.0, color[2] + 0.35),
            1.0 * alpha);
        cairo_pattern_add_color_stop_rgba(slice_grad, 1.0,
            color[0] * 0.85, color[1] * 0.85, color[2] * 0.85,
            0.80 * alpha);

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

        // Crisp border with gradient
        append_annular_sector_path(
            cr, slice_cx, slice_cy,
            slice_r_in, slice_r_out,
            geom.start_angle, geom.end_angle
        );
        cairo_set_source(cr, slice_grad);
        cairo_set_line_width(cr, border);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);

        cairo_pattern_destroy(slice_grad);
        cairo_restore(cr);
    }
}

void ManaCoresSelector::draw_mana_particles(cairo_t* cr, double alpha) {
    if (alpha <= 0.01) return;

    cairo_save(cr);
    for (const auto& p : particles_) {
        if (!p.active || p.life <= 0.0) continue;

        double p_alpha = p.life * alpha;
        if (p_alpha <= 0.01) continue;

        // Soft outer glowing halo
        cairo_arc(cr, p.x, p.y, p.size * 2.2, 0, 2.0 * std::numbers::pi);
        cairo_set_source_rgba(cr, p.r, p.g, p.b, 0.30 * p_alpha);
        cairo_fill(cr);

        // Bright sparkling core
        cairo_arc(cr, p.x, p.y, p.size * 0.75, 0, 2.0 * std::numbers::pi);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.88 * p_alpha);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

void ManaCoresSelector::spawn_particle(
    double x, double y,
    double vx, double vy,
    double r, double g, double b,
    double size, double decay
) {
    for (auto& p : particles_) {
        if (!p.active) {
            p.x = x;
            p.y = y;
            p.vx = vx;
            p.vy = vy;
            p.r = r;
            p.g = g;
            p.b = b;
            p.size = size;
            p.decay = decay;
            p.life = 1.0;
            p.active = true;
            return;
        }
    }
}

void ManaCoresSelector::update_particles(guint64 now_micros, double dt) {
    (void)now_micros;
    // 1. Advance existing particles
    for (auto& p : particles_) {
        if (!p.active) continue;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.life -= p.decay * dt * 60.0;
        if (p.life <= 0.0) {
            p.active = false;
        }
    }

    // 2. Spawn new particles during Idle or Assembling
    if ((state_ == State::Idle || state_ == State::Assembling) && current_alpha_ > 0.3) {
        static double spawn_accum = 0.0;
        spawn_accum += dt;
        while (spawn_accum >= 0.045) { // ~22 particles/sec spawn rate
            spawn_accum -= 0.045;

            double rand_val = static_cast<double>(std::rand()) / RAND_MAX;
            if (rand_val < 0.65 || hovered_radial_ < 0) {
                // Spawn around core perimeter
                double theta = (static_cast<double>(std::rand()) / RAND_MAX) * 2.0 * std::numbers::pi;
                double px = current_cx_ + current_core_radius_ * std::cos(theta);
                double py = current_cy_ + current_core_radius_ * std::sin(theta);
                double speed = 10.0 + 16.0 * (static_cast<double>(std::rand()) / RAND_MAX);
                double vx = std::cos(theta) * speed + (static_cast<double>(std::rand()) / RAND_MAX * 6.0 - 3.0);
                double vy = std::sin(theta) * speed - 12.0; // gentle upward draft

                // 80% cyan-white mana, 20% Aether purple
                double pr = 0.90, pg = 0.95, pb = 1.0;
                if ((std::rand() % 5) == 0) {
                    pr = 0.82; pg = 0.55; pb = 1.0; // Aether
                }
                double size = 1.4 + 1.8 * (static_cast<double>(std::rand()) / RAND_MAX);
                double decay = 0.015 + 0.012 * (static_cast<double>(std::rand()) / RAND_MAX);
                spawn_particle(px, py, vx, vy, pr, pg, pb, size, decay);
            } else {
                // Spawn along hovered slice arc
                const auto& geom = current_slices_[hovered_radial_];
                const auto& col = layout_.kRadialPalette[hovered_radial_];
                double t_interp = static_cast<double>(std::rand()) / RAND_MAX;
                double theta = geom.start_angle + (geom.end_angle - geom.start_angle) * t_interp;
                double r_spawn = current_slice_r_out_;
                double px = current_cx_ + r_spawn * std::cos(theta);
                double py = current_cy_ + r_spawn * std::sin(theta);
                double speed = 12.0 + 15.0 * (static_cast<double>(std::rand()) / RAND_MAX);
                double vx = std::cos(theta) * speed;
                double vy = std::sin(theta) * speed - 8.0;
                double size = 1.4 + 2.0 * (static_cast<double>(std::rand()) / RAND_MAX);
                double decay = 0.018 + 0.012 * (static_cast<double>(std::rand()) / RAND_MAX);
                spawn_particle(px, py, vx, vy, col[0], col[1], col[2], size, decay);
            }
        }
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

    // Glowing border along the inner reveal edge with Aether violet energy flash
    if (mask_radius > 6.0) {
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
        cairo_set_line_width(cr, 3.0);
        cairo_arc(cr, cx, cy, mask_radius, 0, 2.0 * std::numbers::pi);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, 0.82, 0.65, 1.0, 0.45); // Aether violet-cyan aura
        cairo_set_line_width(cr, 20.0);
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
        // 1. Backdrop dim
        draw_backdrop_dim(cr, current_alpha_ * current_wallpaper_alpha_);

        // 2. Realmheart orbital runes
        draw_realmheart_runes(cr, current_cx_, current_cy_, current_core_radius_, current_alpha_ * current_wallpaper_alpha_);

        // 3. Drop shadows (creates elevation)
        draw_drop_shadows(cr, current_cx_, current_cy_, current_core_radius_, current_slice_r_in_, current_slice_r_out_, current_alpha_ * current_wallpaper_alpha_);

        // 4. Core & Slices
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

        // 5. Atmospheric Mana & Aether Particles
        draw_mana_particles(cr, current_alpha_ * current_wallpaper_alpha_);
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
    double dt = (self->last_tick_micros_ > 0)
        ? static_cast<double>(now - self->last_tick_micros_) / 1'000'000.0
        : 0.016;
    self->last_tick_micros_ = now;
    dt = std::clamp(dt, 0.001, 0.05);

    self->update_animations(now);
    self->update_particles(now, dt);
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

void ManaCoresSelector::request_dismiss() {
    if (state_ == State::Assembling || state_ == State::Idle) {
        begin_dismiss();
    }
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
    if (gl_area_) {
        gtk_gl_area_queue_render(GTK_GL_AREA(gl_area_));
    }
    if (canvas_) {
        gtk_widget_queue_draw(canvas_);
    }
}

} // namespace realmheart::mana_core