#include "relictombs/RelictombsOverlay.hpp"

#include "relictombs/RelictombsLayout.hpp"
#include "ui/AssetResolver.hpp"
#include "ui/LayerSurface.hpp"

#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace realmheart::relictombs {
namespace {

constexpr guint kNavigationCooldownMicros = 90'000;

// Phase 4 reconstruction timing (guide §18): 300-450 ms travel, imperceptible
// stagger per piece, then a short complete-arch hold before wallpaper commit.
constexpr double kReconstructTravelMicros = 360'000.0;
constexpr double kReconstructStaggerMicros = 25'000.0;
constexpr double kRepairedHoldMicros = 120'000.0;

constexpr guint kReconstructTickMs = 16;

constexpr const char* kWindowCssClass = "realmheart-relictombs-window";
constexpr const char* kCanvasCssClass = "realmheart-relictombs-root";
constexpr const char* kSurfaceNamespace = "realmheart-relictombs";

// Portal protection factor applied to the wallpaper decode target: the portal
// viewport at the monitor's physical resolution, then a little headroom for
// the apply-state interior motion that later phases add.
constexpr double kDecodeHeadroom = 1.25;

// Decodes one wallpaper with a bounded target so browsing never rescales a
// full 4K original into memory on every Up/Down step.
[[nodiscard]] GdkPixbuf* decode_wallpaper_bounded(
    const std::string& path,
    int target_width,
    int target_height
) {
    GError* error = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_scale(
        path.c_str(),
        std::max(target_width, 1),
        std::max(target_height, 1),
        TRUE,
        &error
    );
    if (pixbuf == nullptr) {
        std::cerr << "[Relictombs] wallpaper decode failed for " << path
                  << ": " << (error != nullptr ? error->message : "unknown")
                  << '\n';
        g_clear_error(&error);
        return nullptr;
    }
    return pixbuf;
}

// Converts a decoded pixbuf into a premultiplied ARGB32 cairo surface
// (byte order converted), so drawing never touches the deprecated
// gdk_cairo_set_source_pixbuf path. Caller owns the result.
[[nodiscard]] cairo_surface_t* pixbuf_to_surface(GdkPixbuf* pixbuf) {
    if (pixbuf == nullptr) return nullptr;
    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    const int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    const gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    if (width <= 0 || height <= 0) return nullptr;

    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_surface_flush(surface);
    unsigned char* dst = cairo_image_surface_get_data(surface);
    const int dst_stride = cairo_image_surface_get_stride(surface);
    const guchar* src = gdk_pixbuf_get_pixels(pixbuf);

    if (!has_alpha) {
        for (int row = 0; row < height; ++row) {
            const guchar* srow = src + row * rowstride;
            auto* drow = reinterpret_cast<guint32*>(dst + row * dst_stride);
            for (int col = 0; col < width; ++col) {
                drow[col] = (0xffu << 24u) |
                            (static_cast<guint32>(srow[col * 3 + 0]) << 16u) |
                            (static_cast<guint32>(srow[col * 3 + 1]) << 8u) |
                            static_cast<guint32>(srow[col * 3 + 2]);
            }
        }
    } else {
        for (int row = 0; row < height; ++row) {
            const guchar* srow = src + row * rowstride;
            auto* drow = reinterpret_cast<guint32*>(dst + row * dst_stride);
            for (int col = 0; col < width; ++col) {
                const guint32 a = srow[col * 4 + 3];
                drow[col] = (a << 24u) |
                            (((static_cast<guint32>(srow[col * 4 + 0]) * a + 127u) / 255u) << 16u) |
                            (((static_cast<guint32>(srow[col * 4 + 1]) * a + 127u) / 255u) << 8u) |
                            ((static_cast<guint32>(srow[col * 4 + 2]) * a + 127u) / 255u);
            }
        }
    }
    cairo_surface_mark_dirty(surface);
    return surface;
}

// Cover-fit draw: fills the destination while preserving aspect, centered.
// Used for the base image (which owns the transparent portal hole).
void draw_cover_fit(
    cairo_t* cr,
    cairo_surface_t* surface,
    double x,
    double y,
    double width,
    double height
) {
    const double image_width =
        static_cast<double>(cairo_image_surface_get_width(surface));
    const double image_height =
        static_cast<double>(cairo_image_surface_get_height(surface));
    if (image_width <= 0.0 || image_height <= 0.0 || width <= 0.0 || height <= 0.0) {
        return;
    }
    const double scale = std::min(width / image_width, height / image_height);
    const double draw_width = image_width * scale;
    const double draw_height = image_height * scale;
    const double offset_x = x + (width - draw_width) * 0.5;
    const double offset_y = y + (height - draw_height) * 0.5;
    cairo_save(cr);
    cairo_translate(cr, offset_x, offset_y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0.0, 0.0);
    cairo_paint(cr);
    cairo_restore(cr);
}

// Cover-fit draw with an additional rotation around the destination center.
// Used for fragment sprites as they lock into their sockets during
// reconstruction; the rotation is authored per-fragment (socket rotation).
void draw_cover_fit_rotated(
    cairo_t* cr,
    cairo_surface_t* surface,
    double x,
    double y,
    double width,
    double height,
    double rotation_deg
) {
    const double image_width =
        static_cast<double>(cairo_image_surface_get_width(surface));
    const double image_height =
        static_cast<double>(cairo_image_surface_get_height(surface));
    if (image_width <= 0.0 || image_height <= 0.0 || width <= 0.0 || height <= 0.0) {
        return;
    }
    const double scale = std::min(width / image_width, height / image_height);
    const double draw_width = image_width * scale;
    const double draw_height = image_height * scale;
    const double center_x = x + width * 0.5;
    const double center_y = y + height * 0.5;
    const double offset_x = x + (width - draw_width) * 0.5;
    const double offset_y = y + (height - draw_height) * 0.5;
    cairo_save(cr);
    cairo_translate(cr, center_x, center_y);
    cairo_rotate(cr, rotation_deg * (G_PI / 180.0));
    cairo_translate(cr, offset_x - center_x, offset_y - center_y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0.0, 0.0);
    cairo_paint(cr);
    cairo_restore(cr);
}

// Cover-crop draw: fills the destination (clipped) while preserving aspect,
// cropping any overflow. Used for the wallpaper inside the portal viewport.
void draw_cover_crop(
    cairo_t* cr,
    cairo_surface_t* surface,
    double x,
    double y,
    double width,
    double height
) {
    const double image_width =
        static_cast<double>(cairo_image_surface_get_width(surface));
    const double image_height =
        static_cast<double>(cairo_image_surface_get_height(surface));
    if (image_width <= 0.0 || image_height <= 0.0 || width <= 0.0 || height <= 0.0) {
        return;
    }
    const double scale = std::max(width / image_width, height / image_height);
    const double draw_width = image_width * scale;
    const double draw_height = image_height * scale;
    const double offset_x = x + (width - draw_width) * 0.5;
    const double offset_y = y + (height - draw_height) * 0.5;

    cairo_save(cr);
    cairo_rectangle(cr, x, y, width, height);
    cairo_clip(cr);
    cairo_translate(cr, offset_x, offset_y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0.0, 0.0);
    cairo_paint(cr);
    cairo_restore(cr);
}

} // namespace

struct RelictombsOverlay::Impl {
    enum class State {
        Closed,
        Browsing,
        Reconstructing,
        Applying,
    };

    GtkApplication* application = nullptr;
    GtkWindow* window = nullptr;
    GtkWidget* canvas = nullptr;
    GtkCssProvider* transparency_provider = nullptr;
    ResultCallback callback;

    State state = State::Closed;

    RelictombsSelection selection;
    std::string selected_path;

    cairo_surface_t* base_surface = nullptr;
    cairo_surface_t* wallpaper_surface = nullptr;
    std::string wallpaper_path;

    // Four broken-arch fragment sprites, decoded once at the active tier and
    // drawn at their authored idle rects (Phase 3: fixed sprites, no motion).
    AssetTier tier = AssetTier::P1080;
    std::array<cairo_surface_t*, kFragmentSpecs.size()> fragment_surfaces{};
    std::array<bool, kFragmentSpecs.size()> fragment_loaded{};

    // Async decode pipeline. A generation token guards stale completions, and
    // the cancellable is swept on teardown.
    GTask* decode_task = nullptr;
    GCancellable* decode_cancel = nullptr;
    guint decode_generation = 0;

    // Decode target in physical pixels, derived from the monitor geometry.
    int decode_target_width = 0;
    int decode_target_height = 0;

    guint64 last_navigation_micros = 0;

    // Set when Enter is pressed before the selected wallpaper has finished
    // decoding; the apply handshake starts the moment the swap lands.
    bool apply_requested = false;

    // Phase 4 reconstruction state. Fragments interpolate idle -> socket over
    // kReconstructionMicros with a staggered start per index; once complete
    // they stay repaired while the wallpaper commit finishes.
    bool repaired = false;
    guint64 reconstruct_started_micros = 0;
    double reconstruct_progress = 1.0;  // 1.0 = fully repaired

    [[nodiscard]] double fragment_progress(std::size_t index) const {
        if (repaired) return 1.0;
        if (reconstruct_started_micros == 0) return 0.0;
        const double elapsed = static_cast<double>(
            g_get_monotonic_time() - reconstruct_started_micros
        );
        const double stagger = static_cast<double>(index) * kReconstructStaggerMicros;
        const double t = (elapsed - stagger) / kReconstructTravelMicros;
        if (t <= 0.0) return 0.0;
        if (t >= 1.0) return 1.0;
        // cubic-bezier(0.16, 1.0, 0.3, 1.0)-like smooth deceleration.
        const double s = 1.0 - t;
        return 1.0 - s * s * s;
    }

    Impl(GtkApplication* app, ResultCallback result_callback)
        : application(app),
          callback(std::move(result_callback)) {}

    ~Impl() {
        if (decode_cancel != nullptr) {
            g_cancellable_cancel(decode_cancel);
        }
        g_clear_object(&decode_task);
        g_clear_object(&decode_cancel);
        cairo_surface_destroy(wallpaper_surface);
        cairo_surface_destroy(base_surface);
        for (cairo_surface_t* surface : fragment_surfaces) {
            cairo_surface_destroy(surface);
        }
        g_clear_object(&transparency_provider);
    }

    void send(RelictombsResultKind kind, std::string payload = {}) {
        if (callback) callback(RelictombsResult{kind, std::move(payload)});
    }

    void present();
    void hide();
    void navigate(int direction);
    void request_apply();
    void request_wallpaper_decode();
    void begin_apply();
    void on_reconstruct_tick();
    void reset_fragments();
    bool handle_key(guint keyval);
    void queue_redraw() const;

    [[nodiscard]] bool wallpaper_is_current() const;
};

struct WallpaperDecodeJob {
    std::string path;
    guint generation = 0;
    int target_width = 0;
    int target_height = 0;
};

void wallpaper_decode_thread(
    GTask* task,
    gpointer source_object,
    gpointer task_data,
    GCancellable* cancellable
) {
    (void)source_object;
    auto* job = static_cast<WallpaperDecodeJob*>(task_data);
    if (job == nullptr) return;

    GdkPixbuf* pixbuf = decode_wallpaper_bounded(
        job->path,
        job->target_width,
        job->target_height
    );
    if (cancellable != nullptr && g_cancellable_is_cancelled(cancellable)) {
        g_clear_object(&pixbuf);
        g_task_return_pointer(task, nullptr, nullptr);
        return;
    }
    if (pixbuf == nullptr) {
        g_task_return_pointer(task, nullptr, nullptr);
        return;
    }
    g_task_return_pointer(task, pixbuf, g_object_unref);
}

void wallpaper_decode_done(GObject* source_object, GAsyncResult* result, gpointer data) {
    (void)source_object;
    auto* impl = static_cast<RelictombsOverlay::Impl*>(data);
    auto* task = G_TASK(result);
    if (impl == nullptr || task == nullptr) return;

    auto* job = static_cast<WallpaperDecodeJob*>(
        g_task_get_task_data(task)
    );
    if (job == nullptr) return;

    const guint generation = job->generation;
    // The worker thread has finished; release the job through its destroy
    // notify exactly once (g_task_set_task_data frees the previous data).
    g_task_set_task_data(task, nullptr, nullptr);

    // Stale completion (a newer wallpaper superseded this one).
    if (generation != impl->decode_generation) return;

    GdkPixbuf* pixbuf = GDK_PIXBUF(g_task_propagate_pointer(task, nullptr));
    if (pixbuf == nullptr) return;

    cairo_surface_t* surface = pixbuf_to_surface(pixbuf);
    g_object_unref(pixbuf);
    if (surface == nullptr) return;

    cairo_surface_destroy(impl->wallpaper_surface);
    impl->wallpaper_surface = surface;
    impl->wallpaper_path = impl->selected_path;
    impl->queue_redraw();

    if (impl->apply_requested && impl->wallpaper_is_current()) {
        impl->apply_requested = false;
        impl->begin_apply();
    }
}

void draw_wallpaper_cb(
    GtkDrawingArea* area,
    cairo_t* cr,
    int width,
    int height,
    gpointer data
) {
    auto* impl = static_cast<RelictombsOverlay::Impl*>(data);
    if (impl == nullptr || area == nullptr || cr == nullptr) return;

    const double framebuffer_width = static_cast<double>(width);
    const double framebuffer_height = static_cast<double>(height);

    // Transparent base: the arch is drawn over the wallpaper, which shows
    // through the portal hole.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // The base image owns the transparent portal hole; the wallpaper must
    // render exactly behind that hole at every tier and aspect. Anchor both
    // draws to one transform: fit the base (scale = min, centered), then map
    // the design-space portal rect through the same scale + offset.
    if (impl->base_surface != nullptr) {
        const double image_width =
            static_cast<double>(cairo_image_surface_get_width(impl->base_surface));
        const double image_height =
            static_cast<double>(cairo_image_surface_get_height(impl->base_surface));
        const double scale = std::min(
            framebuffer_width / image_width,
            framebuffer_height / image_height
        );
        const double draw_width = image_width * scale;
        const double draw_height = image_height * scale;
        const double offset_x = (framebuffer_width - draw_width) * 0.5;
        const double offset_y = (framebuffer_height - draw_height) * 0.5;

        // Draw order is z-order: the wallpaper renders BEHIND the arch, the
        // base image's own alpha carves the portal hole out of it (opaque
        // stone covers the wallpaper; the transparent opening lets it show
        // through), and the fragments float above the arch. Painting the
        // wallpaper after the base would let its square corners cover the
        // stone surround (25% of the portal rect is opaque), so wallpaper
        // goes first.
        if (impl->wallpaper_surface != nullptr) {
            // kPortalViewport is measured against the 1920x1080 design space
            // (== the 1080p tier base). Higher tiers are uniform upscales, so
            // map design -> base-image pixels first, then base-image ->
            // framebuffer through the same fit transform as the base.
            const double design_scale = image_width / kDesignWidth;
            const double portal_x =
                offset_x + kPortalViewport.x * design_scale * scale;
            const double portal_y =
                offset_y + kPortalViewport.y * design_scale * scale;
            const double portal_width =
                kPortalViewport.width * design_scale * scale;
            const double portal_height =
                kPortalViewport.height * design_scale * scale;
            draw_cover_crop(
                cr,
                impl->wallpaper_surface,
                portal_x,
                portal_y,
                portal_width,
                portal_height
            );
        }

        draw_cover_fit(
            cr,
            impl->base_surface,
            0.0,
            0.0,
            framebuffer_width,
            framebuffer_height
        );

        // Phases 3-4: the four broken fragments rest at their authored idle
        // rects and fly to their socket rects during reconstruction. They
        // ride the exact same design-space -> framebuffer mapping as the
        // portal so they can never drift relative to the arch.
        const double design_scale = image_width / kDesignWidth;
        for (std::size_t index = 0;
             index < impl->fragment_surfaces.size();
             ++index) {
            if (!impl->fragment_loaded[index]) continue;
            const auto& spec = kFragmentSpecs[index];
            const double progress = impl->fragment_progress(index);
            const float start_x = spec.idle_rect.x;
            const float start_y = spec.idle_rect.y;
            const float end_x = spec.socket_rect.x;
            const float end_y = spec.socket_rect.y;
            const double fragment_x =
                offset_x +
                (start_x + (end_x - start_x) * progress) * design_scale * scale;
            const double fragment_y =
                offset_y +
                (start_y + (end_y - start_y) * progress) * design_scale * scale;
            const double fragment_width =
                spec.idle_rect.width * design_scale * scale;
            const double fragment_height =
                spec.idle_rect.height * design_scale * scale;
            // Rotate the sprite around its own center while it locks into
            // the socket; idle fragments stay axis-aligned.
            const double rotation_deg =
                spec.socket_rotation_deg * progress;
            if (std::abs(rotation_deg) > 0.1) {
                draw_cover_fit_rotated(
                    cr,
                    impl->fragment_surfaces[index],
                    fragment_x,
                    fragment_y,
                    fragment_width,
                    fragment_height,
                    rotation_deg
                );
            } else {
                draw_cover_fit(
                    cr,
                    impl->fragment_surfaces[index],
                    fragment_x,
                    fragment_y,
                    fragment_width,
                    fragment_height
                );
            }
        }
    }
}

void install_transparency_css(RelictombsOverlay::Impl& impl) {
    GdkDisplay* display = gdk_display_get_default();
    if (display == nullptr) return;

    impl.transparency_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        impl.transparency_provider,
        R"CSS(
            window.realmheart-relictombs-window,
            window.realmheart-relictombs-window > *,
            .realmheart-relictombs-root {
                background-color: rgba(0, 0, 0, 0);
                background-image: none;
                border: none;
                box-shadow: none;
            }
        )CSS"
    );
    gtk_style_context_add_provider_for_display(
        display,
        GTK_STYLE_PROVIDER(impl.transparency_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER + 1
    );
}

gboolean canvas_key_pressed(
    GtkEventControllerKey* controller,
    guint keyval,
    guint keycode,
    GdkModifierType state,
    gpointer data
) {
    (void)controller;
    (void)keycode;
    (void)state;
    auto* impl = static_cast<RelictombsOverlay::Impl*>(data);
    if (impl == nullptr) return GDK_EVENT_STOP;
    if (impl->handle_key(keyval)) return GDK_EVENT_STOP;
    return GDK_EVENT_PROPAGATE;
}

void RelictombsOverlay::Impl::queue_redraw() const {
    if (canvas != nullptr) {
        gtk_widget_queue_draw(canvas);
    }
}

void RelictombsOverlay::Impl::present() {
    if (window == nullptr) return;
    gtk_window_present(window);
    gtk_widget_grab_focus(canvas);
}

void RelictombsOverlay::Impl::hide() {
    if (window == nullptr) return;
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    queue_redraw();
}

void RelictombsOverlay::Impl::request_wallpaper_decode() {
    if (selected_path.empty()) return;

    g_cancellable_cancel(decode_cancel);
    g_clear_object(&decode_task);
    ++decode_generation;

    if (decode_cancel == nullptr) {
        decode_cancel = g_cancellable_new();
    }
    auto* job = new WallpaperDecodeJob{
        selected_path,
        decode_generation,
        decode_target_width,
        decode_target_height
    };
    decode_task = g_task_new(
        nullptr,
        decode_cancel,
        &wallpaper_decode_done,
        this
    );
    g_task_set_task_data(decode_task, job, +[](gpointer pointer) {
        delete static_cast<WallpaperDecodeJob*>(pointer);
    });
    g_task_run_in_thread(decode_task, &wallpaper_decode_thread);
}

bool RelictombsOverlay::Impl::wallpaper_is_current() const {
    return !wallpaper_path.empty() && wallpaper_path == selected_path;
}

void RelictombsOverlay::Impl::navigate(int direction) {
    if (state != State::Browsing) return;
    if (direction == 0) return;

    const guint64 now = g_get_monotonic_time();
    if (now - last_navigation_micros < kNavigationCooldownMicros) return;
    last_navigation_micros = now;

    apply_requested = false;
    if (!selection.navigate(direction)) return;
    selected_path = selection.selected().string();
    std::cerr << "[Relictombs] browsing: "
              << (direction > 0 ? "down" : "up") << " -> " << selected_path << '\n';
    request_wallpaper_decode();
}

void RelictombsOverlay::Impl::begin_apply() {
    if (state != State::Browsing && state != State::Applying) return;
    if (!wallpaper_is_current()) return;

    // Enter: the selected wallpaper is ready. Lock navigation, stop the
    // idle float, and play the signature Broken Arch reconstruction
    // (fragments fly idle -> socket, staggered, smooth deceleration). The
    // wallpaper commit result only fires once the arch is fully repaired
    // plus a short complete-arch hold (guide §16-§20).
    state = State::Reconstructing;
    apply_requested = false;
    repaired = false;
    reconstruct_started_micros = g_get_monotonic_time();
    reconstruct_progress = 0.0;
    std::cerr << "[Relictombs] reconstruction started\n";
    g_timeout_add(kReconstructTickMs, +[](gpointer data) -> gboolean {
        auto* impl = static_cast<RelictombsOverlay::Impl*>(data);
        if (impl == nullptr) return G_SOURCE_REMOVE;
        impl->on_reconstruct_tick();
        return G_SOURCE_CONTINUE;
    }, this);
}

void RelictombsOverlay::Impl::on_reconstruct_tick() {
    if (state != State::Reconstructing) return;

    const guint64 now = g_get_monotonic_time();
    const double elapsed =
        static_cast<double>(now - reconstruct_started_micros);
    // All four fragments landed once the last stagger + travel completes.
    const double total = kReconstructTravelMicros +
                         (kFragmentSpecs.size() - 1) * kReconstructStaggerMicros;
    reconstruct_progress = std::clamp(elapsed / total, 0.0, 1.0);
    queue_redraw();

    if (elapsed >= total + kRepairedHoldMicros) {
        repaired = true;
        reconstruct_progress = 1.0;
        state = State::Applying;
        queue_redraw();
        std::cerr << "[Relictombs] arch repaired, committing\n";
        send(RelictombsResultKind::Apply, selected_path);
    }
}

void RelictombsOverlay::Impl::reset_fragments() {
    repaired = false;
    reconstruct_started_micros = 0;
    reconstruct_progress = 1.0;
    queue_redraw();
}

void RelictombsOverlay::Impl::request_apply() {
    if (state != State::Browsing) return;
    if (wallpaper_is_current()) {
        begin_apply();
        return;
    }
    // Selected wallpaper still decoding: remember the request; the decode
    // completion path starts the handshake the moment the swap lands.
    apply_requested = true;
}

bool RelictombsOverlay::Impl::handle_key(guint keyval) {
    switch (keyval) {
    case GDK_KEY_Up:
    case GDK_KEY_k:
        navigate(-1);
        return true;
    case GDK_KEY_Down:
    case GDK_KEY_j:
        navigate(1);
        return true;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_space:
        request_apply();
        return true;
    case GDK_KEY_Escape:
        if (state == State::Browsing) {
            send(RelictombsResultKind::Cancel);
            hide();
            state = State::Closed;
            reset_fragments();
        }
        return true;
    case GDK_KEY_x:
    case GDK_KEY_X:
        // Phase 4 debug toggle: instantly flip between idle fragments and the
        // fully repaired arch. Lets socket geometry be tuned without waiting
        // on the reconstruction animation (guide §36 Phase 4).
        if (state == State::Browsing) {
            repaired = !repaired;
            reconstruct_started_micros = 0;
            reconstruct_progress = repaired ? 1.0 : 0.0;
            std::cerr << "[Relictombs] debug toggle: "
                      << (repaired ? "repaired" : "idle") << '\n';
            queue_redraw();
        }
        return true;
    default:
        return false;
    }
}

RelictombsOverlay::RelictombsOverlay(
    GtkApplication* application,
    ResultCallback callback
)
    : impl_(std::make_unique<Impl>(application, std::move(callback))) {}

RelictombsOverlay::~RelictombsOverlay() = default;

bool RelictombsOverlay::prepare(std::string* error) {
    if (impl_->window != nullptr) return true;

    GtkWindow* window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(window, "Relictombs");
    gtk_window_set_resizable(window, FALSE);
    gtk_window_set_decorated(window, FALSE);

    GtkWidget* canvas = gtk_drawing_area_new();
    gtk_widget_set_name(canvas, kCanvasCssClass);
    gtk_widget_add_css_class(canvas, kCanvasCssClass);
    gtk_widget_set_focusable(canvas, TRUE);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(canvas),
        &draw_wallpaper_cb,
        impl_.get(),
        nullptr
    );
    gtk_window_set_child(window, canvas);

    // Fullscreen overlay above every other surface, exclusive keyboard so
    // Up/Down/Enter/Escape drive the selector directly.
    ui::LayerSurfaceSpec spec = ui::make_layer_surface_spec(
        kSurfaceNamespace,
        ui::LayerSurfaceLevel::Overlay,
        ui::LayerKeyboardMode::Exclusive
    );
    spec.anchor_left = true;
    spec.exclusive_zone = -1;
    ui::apply_layer_surface(window, spec);

    GdkMonitor* monitor = ui::resolve_layer_surface_monitor(
        GTK_WIDGET(window),
        -1
    );
    if (monitor == nullptr) {
        if (error != nullptr) *error = "no output available for Relictombs";
        gtk_window_destroy(window);
        return false;
    }

    GdkRectangle geometry{};
    gdk_monitor_get_geometry(monitor, &geometry);
    const int scale_factor = gdk_monitor_get_scale_factor(monitor);
    const int physical_height = geometry.height * scale_factor;
    gtk_window_set_default_size(window, geometry.width, geometry.height);
    g_object_unref(monitor);

    // Decode target matches the portal viewport at physical resolution.
    const SceneTransform transform = make_scene_transform(
        static_cast<float>(geometry.width),
        static_cast<float>(geometry.height)
    );
    impl_->decode_target_width = static_cast<int>(
        std::lround(kPortalViewport.width * transform.scale * kDecodeHeadroom)
    );
    impl_->decode_target_height = static_cast<int>(
        std::lround(kPortalViewport.height * transform.scale * kDecodeHeadroom)
    );

    gtk_widget_add_css_class(GTK_WIDGET(window), kWindowCssClass);
    install_transparency_css(*impl_);

    GtkEventController* key_controller =
        GTK_EVENT_CONTROLLER(gtk_event_controller_key_new());
    g_signal_connect(
        key_controller,
        "key-pressed",
        G_CALLBACK(&canvas_key_pressed),
        impl_.get()
    );
    gtk_widget_add_controller(GTK_WIDGET(window), key_controller);

    // Load the tiered base arch image.
    const AssetTier tier = select_asset_tier(physical_height);
    impl_->tier = tier;
    const auto base_path = ui::resolve_project_asset(
        std::string("Relictombs-Broken_Arch/") +
        std::string(base_asset_relative_path(tier))
    );
    if (!base_path) {
        if (error != nullptr) {
            *error = "Relictombs base asset is unavailable for tier " +
                     std::to_string(physical_height);
        }
        gtk_window_destroy(window);
        return false;
    }

    GError* decode_error = nullptr;
    GdkPixbuf* base_pixbuf = gdk_pixbuf_new_from_file(
        base_path->c_str(),
        &decode_error
    );
    if (base_pixbuf == nullptr) {
        if (error != nullptr) {
            *error = "Relictombs base asset failed to decode: " +
                     std::string(decode_error != nullptr
                                     ? decode_error->message
                                     : "unknown error");
        }
        g_clear_error(&decode_error);
        gtk_window_destroy(window);
        return false;
    }
    impl_->base_surface = pixbuf_to_surface(base_pixbuf);
    g_object_unref(base_pixbuf);
    if (impl_->base_surface == nullptr) {
        if (error != nullptr) {
            *error = "Relictombs base asset failed to convert to surface";
        }
        gtk_window_destroy(window);
        return false;
    }

    // Phase 3: decode the four broken fragments at the active tier. A missing
    // fragment degrades gracefully (logs, stays absent) instead of failing the
    // whole selector: the arch is still fully usable without loose rocks.
    for (std::size_t index = 0; index < kFragmentSpecs.size(); ++index) {
        const auto relative = fragment_asset_relative_path(
            tier,
            kFragmentSpecs[index]
        );
        const auto fragment_path = ui::resolve_project_asset(
            std::string("Relictombs-Broken_Arch/") + relative
        );
        if (!fragment_path) {
            std::cerr << "[Relictombs] fragment asset unavailable: "
                      << relative << '\n';
            continue;
        }

        GError* fragment_error = nullptr;
        GdkPixbuf* fragment_pixbuf = gdk_pixbuf_new_from_file(
            fragment_path->c_str(),
            &fragment_error
        );
        if (fragment_pixbuf == nullptr) {
            std::cerr << "[Relictombs] fragment asset failed to decode: "
                      << relative << ": "
                      << (fragment_error != nullptr
                              ? fragment_error->message
                              : "unknown error")
                      << '\n';
            g_clear_error(&fragment_error);
            continue;
        }
        cairo_surface_t* fragment_surface =
            pixbuf_to_surface(fragment_pixbuf);
        g_object_unref(fragment_pixbuf);
        if (fragment_surface == nullptr) {
            std::cerr << "[Relictombs] fragment asset failed to convert: "
                      << relative << '\n';
            continue;
        }
        cairo_surface_destroy(impl_->fragment_surfaces[index]);
        impl_->fragment_surfaces[index] = fragment_surface;
        impl_->fragment_loaded[index] = true;
    }
    std::size_t loaded_fragments = 0;
    for (const bool loaded : impl_->fragment_loaded) {
        if (loaded) ++loaded_fragments;
    }
    std::cerr << "[Relictombs] fragments loaded: " << loaded_fragments
              << "/" << kFragmentSpecs.size() << '\n';

    impl_->window = window;
    impl_->canvas = canvas;
    impl_->state = Impl::State::Closed;
    std::cerr << "[Relictombs] overlay prepared (tier "
              << static_cast<int>(tier) << ", portal decode target "
              << impl_->decode_target_width << "x"
              << impl_->decode_target_height << ")\n";
    return true;
}

bool RelictombsOverlay::preload(
    const RelictombsSelection& selection,
    std::string* error
) {
    if (impl_->window == nullptr) {
        if (error != nullptr) *error = "Relictombs overlay is not prepared";
        return false;
    }
    if (selection.candidate_count() == 0) {
        if (error != nullptr) *error = "Relictombs selection is empty";
        return false;
    }
    if (impl_->state != Impl::State::Closed) {
        if (error != nullptr) *error = "Relictombs session is already active";
        return false;
    }

    impl_->selection = selection;
    impl_->selected_path = selection.selected().string();
    impl_->apply_requested = false;
    impl_->reset_fragments();
    impl_->request_wallpaper_decode();
    return true;
}

bool RelictombsOverlay::show(
    RelictombsSelection selection,
    std::string* error
) {
    if (impl_->window == nullptr) {
        if (error != nullptr) *error = "Relictombs overlay is not prepared";
        return false;
    }
    if (selection.candidate_count() == 0) {
        if (error != nullptr) *error = "Relictombs selection is empty";
        return false;
    }

    impl_->selection = std::move(selection);
    impl_->selected_path = impl_->selection.selected().string();
    impl_->apply_requested = false;
    impl_->state = Impl::State::Browsing;
    impl_->last_navigation_micros = g_get_monotonic_time();
    impl_->reset_fragments();
    impl_->request_wallpaper_decode();
    impl_->present();
    std::cerr << "[Relictombs] session open: " << impl_->selected_path << '\n';
    return true;
}

void RelictombsOverlay::cancel() {
    if (impl_->state == Impl::State::Browsing) {
        impl_->send(RelictombsResultKind::Cancel);
        impl_->hide();
        impl_->state = Impl::State::Closed;
        impl_->reset_fragments();
    }
    // Applying owns its lifecycle; Closed is a no-op.
}

void RelictombsOverlay::backend_prepared() {
    if (impl_->state != Impl::State::Applying) return;
    impl_->send(RelictombsResultKind::Commit, impl_->selected_path);
}

void RelictombsOverlay::backend_committed() {
    if (impl_->state != Impl::State::Applying) return;
    impl_->send(RelictombsResultKind::Complete);
    impl_->hide();
    impl_->state = Impl::State::Closed;
    impl_->reset_fragments();
}

void RelictombsOverlay::backend_failed(std::string_view diagnostic) {
    if (impl_->state != Impl::State::Applying) return;
    impl_->send(RelictombsResultKind::Error, std::string(diagnostic));
    impl_->hide();
    impl_->state = Impl::State::Closed;
    impl_->reset_fragments();
}

bool RelictombsOverlay::active() const noexcept {
    return impl_->state == Impl::State::Browsing ||
           impl_->state == Impl::State::Reconstructing ||
           impl_->state == Impl::State::Applying;
}

} // namespace realmheart::relictombs