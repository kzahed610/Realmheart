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
// Selector zoom: the base arch renders 20% larger than cover-fit so the portal
// fills more of the screen and the wallpaper inside is immediately recognisable.
// The base is scaled around the framebuffer center; edges crop naturally.
constexpr double kZoomFactor = 1.2;
// One short understated line for a commit failure that keeps the selector
// open (guide §25); anything longer gets truncated.
constexpr std::size_t kErrorLineMaxChars = 96;

// Phase 5: Idle floating animation (guide §11)
constexpr double kIdleFloatPeriodMicros[] = {4'000'000.0, 5'000'000.0, 3'500'000.0, 4'500'000.0};
constexpr double kIdleFloatAmplitudeX[] = {1.5, 2.0, 1.0, 2.5};
constexpr double kIdleFloatAmplitudeY[] = {3.0, 2.5, 4.0, 3.5};
constexpr double kIdleFloatRotationAmp[] = {0.8, 1.2, 0.6, 1.0};
constexpr double kIdleFloatPhaseOffset[] = {0.0, 1.57, 3.14, 4.71};

// Opening sequence timing (guide §13)
constexpr double kOpeningDurationMicros = 300'000.0;
constexpr double kOpeningBaseScaleStart = 0.97;
constexpr double kOpeningFragmentBias = 0.15;  // start 15% toward socket

// Phase 9: Exit transition (guide §22)
constexpr double kExitDurationMicros = 250'000.0;
constexpr double kExitPortalBrightenMicros = 100'000.0;

// Phase 10: FX (guide §32)
constexpr std::size_t kNumMotes = 8;
constexpr double kMoteLifetimeMicros = 1'500'000.0;
constexpr double kMoteSpeedMin = 15.0;
constexpr double kMoteSpeedMax = 40.0;
constexpr double kMoteSizeMin = 1.5;
constexpr double kMoteSizeMax = 3.5;

// Joining flash during reconstruction
constexpr double kJoinFlashDurationMicros = 50'000.0;

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
        Exiting,
    };

    GtkApplication* application = nullptr;
    GtkWindow* window = nullptr;
    GtkWidget* canvas = nullptr;
    GtkCssProvider* transparency_provider = nullptr;
    ResultCallback callback;

    State state = RelictombsOverlay::Impl::State::Closed;

    RelictombsSelection selection;
    std::string selected_path;

    cairo_surface_t* base_surface = nullptr;
    cairo_surface_t* wallpaper_surface = nullptr;
    cairo_surface_t* previous_wallpaper_surface = nullptr;
    guint64 nav_transition_started_micros = 0;
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

    // Understated one-line diagnostic shown while a commit failure keeps the
    // selector open (guide §25). Cleared on the next navigation/apply.
    std::string error_message;

    // Phase 5: Idle floating animation state
    guint64 idle_animation_start_micros = 0;

    // Opening sequence state (guide §13)
    bool opening_animation_active = false;
    guint64 opening_started_micros = 0;

    // Phase 9: Exit transition state (guide §22)
    bool exit_animation_active = false;
    guint64 exit_started_micros = 0;

    // Phase 10: FX state
    struct Mote {
        double x = 0.0, y = 0.0;
        double vx = 0.0, vy = 0.0;
        double size = 2.0;
        double life_micros = 0.0;
        double max_life_micros = 1.0;
    };
    std::array<Mote, kNumMotes> motes{};
    bool motes_initialized = false;

    // Joining flash during reconstruction
    guint64 join_flash_start_micros = 0;

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
        cairo_surface_destroy(previous_wallpaper_surface);
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
    void update_idle_animation();
    void start_opening_animation();
    void start_exit_animation();
    void update_motes(double dt);
    void render_fx(cairo_t* cr, double offset_x, double offset_y, double design_scale, double scale);
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

    if (impl->wallpaper_surface != nullptr) {
        cairo_surface_destroy(impl->previous_wallpaper_surface);
        impl->previous_wallpaper_surface = impl->wallpaper_surface;
        impl->nav_transition_started_micros = g_get_monotonic_time();
    }
    impl->wallpaper_surface = surface;
    impl->wallpaper_path = impl->selected_path;
    impl->queue_redraw();

    if (impl->apply_requested && impl->wallpaper_is_current()) {
        impl->apply_requested = false;
        impl->begin_apply();
    }

    // Start opening animation on first wallpaper load
    if (impl->state == RelictombsOverlay::Impl::State::Browsing && impl->idle_animation_start_micros == 0) {
        impl->start_opening_animation();
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
    double image_width = 0.0, image_height = 0.0;
    double cover_scale = 1.0, scale = 1.0, draw_width = 0.0, draw_height = 0.0;
    double offset_x = 0.0, offset_y = 0.0, design_scale = 1.0;

    if (impl->base_surface != nullptr) {
        image_width =
            static_cast<double>(cairo_image_surface_get_width(impl->base_surface));
        image_height =
            static_cast<double>(cairo_image_surface_get_height(impl->base_surface));
        // Cover-fit gives uniform scale so the base fills the framebuffer with
        // no letterbox. The selector zoom (kZoomFactor) grows the arch 20%
        // beyond that, cropping edges so the portal is more recognisable.
        cover_scale = std::min(
            framebuffer_width / image_width,
            framebuffer_height / image_height
        );
        scale = cover_scale * kZoomFactor;
        draw_width = image_width * scale;
        draw_height = image_height * scale;
        // Center the zoomed arch in the framebuffer.
        offset_x = (framebuffer_width - draw_width) * 0.5;
        offset_y = ((framebuffer_height - draw_height) * 0.5) + 50.0; // Shift down 50px per user request
        design_scale = image_width / kDesignWidth;

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

            constexpr double kNavCrossfadeMicros = 200'000.0;
            double progress = 1.0;
            if (impl->previous_wallpaper_surface != nullptr && impl->nav_transition_started_micros > 0) {
                const double elapsed = static_cast<double>(
                    g_get_monotonic_time() - impl->nav_transition_started_micros
                );
                progress = elapsed / kNavCrossfadeMicros;
                if (progress >= 1.0) {
                    progress = 1.0;
                    cairo_surface_destroy(impl->previous_wallpaper_surface);
                    impl->previous_wallpaper_surface = nullptr;
                } else {
                    impl->queue_redraw();
                }
            }

            const double ease_p = progress * (2.0 - progress);

            if (impl->previous_wallpaper_surface != nullptr && progress < 1.0) {
                cairo_save(cr);
                cairo_push_group(cr);
                draw_cover_crop(
                    cr,
                    impl->previous_wallpaper_surface,
                    portal_x,
                    portal_y,
                    portal_width,
                    portal_height
                );
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, 1.0 - ease_p);
                cairo_restore(cr);
            }

            cairo_save(cr);
            cairo_push_group(cr);
            draw_cover_crop(
                cr,
                impl->wallpaper_surface,
                portal_x,
                portal_y,
                portal_width,
                portal_height
            );
            cairo_pop_group_to_source(cr);
            if (progress < 1.0) {
                cairo_paint_with_alpha(cr, ease_p);
            } else {
                cairo_paint(cr);
            }
            cairo_restore(cr);
        }

        // Draw the base arch with the zoomed transform so it renders 20%
        // larger than cover-fit, cropping edges. We do NOT use draw_cover_fit
        // here because it recomputes its own scale; the zoom factor must be
        // part of the same transform as the portal and fragments.
        cairo_save(cr);
        cairo_translate(cr, offset_x, offset_y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, impl->base_surface, 0.0, 0.0);
        cairo_paint(cr);
        cairo_restore(cr);

        // Phases 3-4: the four broken fragments rest at their authored idle
        // rects and fly to their socket rects during reconstruction. They
        // ride the exact same design-space -> framebuffer mapping as the
        // portal so they can never drift relative to the arch.
        const double design_scale = image_width / kDesignWidth;

        // Phase 5: Idle floating animation + Opening sequence (guide §11, §13)
        double idle_time_sec = 0.0;
        double opening_progress = 0.0;
        if (impl->idle_animation_start_micros > 0) {
            idle_time_sec = static_cast<double>(g_get_monotonic_time() - impl->idle_animation_start_micros) / 1'000'000.0;
        }
        if (impl->opening_animation_active) {
            double elapsed = static_cast<double>(g_get_monotonic_time() - impl->opening_started_micros);
            opening_progress = std::clamp(elapsed / kOpeningDurationMicros, 0.0, 1.0);
            if (opening_progress >= 1.0) {
                impl->opening_animation_active = false;
                impl->opening_started_micros = 0;
            }
        }

        for (std::size_t index = 0;
             index < impl->fragment_surfaces.size();
             ++index) {
            if (!impl->fragment_loaded[index]) continue;
            const auto& spec = kFragmentSpecs[index];
            const double recon_progress = impl->fragment_progress(index);

            // Opening: fragments start biased toward socket, float to idle
            double open_bias = 0.0;
            if (impl->opening_animation_active || (impl->state == RelictombsOverlay::Impl::State::Browsing && opening_progress < 1.0)) {
                // Ease-out for opening: start at kOpeningFragmentBias toward socket, go to 0 (idle)
                double open_ease = 1.0 - (1.0 - opening_progress) * (1.0 - opening_progress);
                open_bias = kOpeningFragmentBias * (1.0 - open_ease);
            }

            // Interpolate: idle + open_bias*(socket - idle) - recon_progress*(idle + open_bias*(socket-idle) - socket)
            // = idle*(1-recon_progress) + socket*recon_progress + open_bias*(1-recon_progress)*(socket-idle)
            const float start_x = spec.idle_rect.x;
            const float start_y = spec.idle_rect.y;
            const float end_x = spec.socket_rect.x;
            const float end_y = spec.socket_rect.y;

            double interp_x = start_x + (end_x - start_x) * recon_progress + open_bias * (end_x - start_x) * (1.0 - recon_progress);
            double interp_y = start_y + (end_y - start_y) * recon_progress + open_bias * (end_y - start_y) * (1.0 - recon_progress);

            // Phase 5: Idle floating motion (guide §11) - only when browsing and not reconstructing
            double float_offset_x = 0.0, float_offset_y = 0.0, float_rotation = 0.0;
            if (impl->state == RelictombsOverlay::Impl::State::Browsing && recon_progress == 0.0 && !impl->opening_animation_active) {
                double period = kIdleFloatPeriodMicros[index] / 1'000'000.0;
                double phase = kIdleFloatPhaseOffset[index];
                float_offset_x = std::sin(idle_time_sec * 2.0 * G_PI / period + phase) * kIdleFloatAmplitudeX[index];
                float_offset_y = std::sin(idle_time_sec * 2.0 * G_PI / period * 0.73 + phase) * kIdleFloatAmplitudeY[index];
                float_rotation = std::sin(idle_time_sec * 2.0 * G_PI / period * 0.51 + phase) * kIdleFloatRotationAmp[index];
            }

            const double fragment_x =
                offset_x + (interp_x + float_offset_x) * design_scale * scale;
            const double fragment_y =
                offset_y + (interp_y + float_offset_y) * design_scale * scale;
            const double fragment_width =
                spec.idle_rect.width * design_scale * scale;
            const double fragment_height =
                spec.idle_rect.height * design_scale * scale;

            // Rotation: reconstruction rotation + idle floating rotation
            const double rotation_deg = spec.socket_rotation_deg * recon_progress + float_rotation;
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

    // Guide §25: one short understated diagnostic, bottom-center, so a
    // failed commit explains itself without stealing the scene.
    if (!impl->error_message.empty()) {
        cairo_save(cr);
        cairo_select_font_face(
            cr,
            "Sans",
            CAIRO_FONT_SLANT_NORMAL,
            CAIRO_FONT_WEIGHT_NORMAL
        );
        cairo_set_font_size(cr, 16.0);
        cairo_text_extents_t extents{};
        cairo_text_extents(cr, impl->error_message.c_str(), &extents);
        const double text_x =
            (framebuffer_width - extents.width) * 0.5 - extents.x_bearing;
        const double text_y = framebuffer_height - 24.0;
        // Subtle: dim text over a soft dark scrim, gold-tinted to match the
        // realm's palette.
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
        cairo_rectangle(
            cr,
            text_x - 12.0,
            text_y - extents.height + 6.0,
            extents.width + 24.0,
            extents.height + 12.0
        );
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.93, 0.79, 0.45, 0.85);
        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, impl->error_message.c_str());
        cairo_restore(cr);
    }

    // Phase 10: Render FX (guide §32)
    impl->render_fx(cr, offset_x, offset_y, design_scale, scale);

    // Phase 9: Exit transition - fade out Relictombs to reveal new desktop (guide §22)
    if (impl->exit_animation_active) {
        double elapsed = static_cast<double>(g_get_monotonic_time() - impl->exit_started_micros);
        double exit_progress = std::clamp(elapsed / kExitDurationMicros, 0.0, 1.0);
        if (exit_progress >= 1.0) {
            impl->exit_animation_active = false;
            impl->exit_started_micros = 0;
            // Close the selector
            impl->hide();
            impl->state = RelictombsOverlay::Impl::State::Closed;
            impl->reset_fragments();
            if (impl->state == RelictombsOverlay::Impl::State::Exiting) {
                impl->send(RelictombsResultKind::Cancel, "");
            }
            return;
        }

        // Portal brightens first (guide §22) - subtle warm glow at portal
        if (elapsed < kExitPortalBrightenMicros) {
            double portal_brighten = 1.0 - std::clamp(elapsed / kExitPortalBrightenMicros, 0.0, 1.0);
            double portal_x = offset_x + kPortalViewport.x * design_scale * scale;
            double portal_y = offset_y + kPortalViewport.y * design_scale * scale;
            double portal_width = kPortalViewport.width * design_scale * scale;
            double portal_height = kPortalViewport.height * design_scale * scale;
            double radius = std::max(portal_width, portal_height) * 0.7;

            cairo_save(cr);
            cairo_set_source_rgba(cr, 1.0, 0.9, 0.5, portal_brighten * 0.4);
            cairo_arc(cr, portal_x + portal_width * 0.5, portal_y + portal_height * 0.5, radius, 0, 2.0 * G_PI);
            cairo_fill(cr);
            cairo_restore(cr);
        }

        // Fade out entire Relictombs scene with ease-out
        double fade_out = exit_progress * (2.0 - exit_progress); // quadratic ease-out
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, fade_out);
        cairo_paint(cr);
        cairo_restore(cr);
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
    std::cerr << "[Relictombs] canvas_key_pressed: " << keyval << " data=" << data << "\n";
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
    std::cerr << "[Relictombs] present() called\n";
    gtk_window_present(window);
    std::cerr << "[Relictombs] window presented, grabbing focus on canvas: " << canvas << "\n";
    gtk_widget_grab_focus(canvas);
}

void RelictombsOverlay::Impl::hide() {
    if (window == nullptr) return;
    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    queue_redraw();
}

void RelictombsOverlay::Impl::request_wallpaper_decode() {
    if (selected_path.empty()) return;

    // A cancellable is single-shot: once cancelled it stays cancelled, and
    // handing a dead cancellable to the next decode would make every
    // subsequent navigation silently no-op (the worker sees
    // g_cancellable_is_cancelled() immediately and returns null). Clear the
    // old handle so each decode starts with a fresh, live cancellable.
    g_cancellable_cancel(decode_cancel);
    g_clear_object(&decode_cancel);
    g_clear_object(&decode_task);
    ++decode_generation;

    decode_cancel = g_cancellable_new();
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
    if (state != RelictombsOverlay::Impl::State::Browsing) return;
    if (direction == 0) return;

    const guint64 now = g_get_monotonic_time();
    if (now - last_navigation_micros < kNavigationCooldownMicros) return;
    last_navigation_micros = now;

    apply_requested = false;
    error_message.clear();
    if (!selection.navigate(direction)) return;
    selected_path = selection.selected().string();
    std::cerr << "[Relictombs] browsing: "
              << (direction > 0 ? "down" : "up") << " -> " << selected_path << '\n';
    request_wallpaper_decode();
}

void RelictombsOverlay::Impl::begin_apply() {
    if (state != RelictombsOverlay::Impl::State::Browsing && state != RelictombsOverlay::Impl::State::Applying) return;
    if (!wallpaper_is_current()) return;

    // Enter: the selected wallpaper is ready. Lock navigation, stop the
    // idle float, and play the signature Broken Arch reconstruction
    // (fragments fly idle -> socket, staggered, smooth deceleration). The
    // wallpaper commit result only fires once the arch is fully repaired
    // plus a short complete-arch hold (guide §16-§20).
    state = RelictombsOverlay::Impl::State::Reconstructing;
    apply_requested = false;
    error_message.clear();
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
    if (state != RelictombsOverlay::Impl::State::Reconstructing) return;

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
        state = RelictombsOverlay::Impl::State::Applying;
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

// Phase 5: Update idle floating animation (guide §11)
void RelictombsOverlay::Impl::update_idle_animation() {
    if (state != RelictombsOverlay::Impl::State::Browsing) return;
    if (idle_animation_start_micros == 0) {
        idle_animation_start_micros = g_get_monotonic_time();
    }
    queue_redraw();
}

// Idle animation frame callback - runs at monitor refresh rate
gboolean idle_animation_tick(GtkWidget* widget G_GNUC_UNUSED, GdkFrameClock* frame_clock G_GNUC_UNUSED, gpointer user_data) {
    auto* impl = static_cast<RelictombsOverlay::Impl*>(user_data);
    if (impl == nullptr) return G_SOURCE_REMOVE;
    impl->update_idle_animation();
    return G_SOURCE_CONTINUE;
}

// Opening sequence: fragments start socket-biased, float to idle (guide §13)
void RelictombsOverlay::Impl::start_opening_animation() {
    opening_animation_active = true;
    opening_started_micros = g_get_monotonic_time();
    idle_animation_start_micros = g_get_monotonic_time();
    queue_redraw();
}

// Phase 9: Exit transition - fade Relictombs away (guide §22)
void RelictombsOverlay::Impl::start_exit_animation() {
    exit_animation_active = true;
    exit_started_micros = g_get_monotonic_time();
    state = RelictombsOverlay::Impl::State::Exiting;
    queue_redraw();
}

// Phase 10: Update motes (guide §32)
void RelictombsOverlay::Impl::update_motes(double dt) {
    (void)g_get_monotonic_time();
    if (!motes_initialized) {
        // Initialize motes around the portal center
        for (std::size_t i = 0; i < kNumMotes; ++i) {
            double angle = (static_cast<double>(i) / kNumMotes) * 2.0 * G_PI;
            double radius = 50.0 + (i % 3) * 30.0;
            motes[i].x = std::cos(angle) * radius;
            motes[i].y = std::sin(angle) * radius;
            double speed = kMoteSpeedMin + (i % 5) * ((kMoteSpeedMax - kMoteSpeedMin) / 4.0);
            motes[i].vx = std::cos(angle) * speed;
            motes[i].vy = std::sin(angle) * speed;
            motes[i].size = kMoteSizeMin + (i % 4) * ((kMoteSizeMax - kMoteSizeMin) / 3.0);
            motes[i].life_micros = 0.0;
            motes[i].max_life_micros = kMoteLifetimeMicros;
        }
        motes_initialized = true;
    }

    // Update motes
    for (auto& mote : motes) {
        mote.life_micros += dt * 1'000'000.0;
        if (mote.life_micros >= mote.max_life_micros) {
            // Respawn at portal center with new random direction
            double angle = (static_cast<double>(rand()) / RAND_MAX) * 2.0 * G_PI;
            double radius = 20.0 + (static_cast<double>(rand()) / RAND_MAX) * 40.0;
            mote.x = std::cos(angle) * radius;
            mote.y = std::sin(angle) * radius;
            double speed = kMoteSpeedMin + (static_cast<double>(rand()) / RAND_MAX) * (kMoteSpeedMax - kMoteSpeedMin);
            mote.vx = std::cos(angle) * speed;
            mote.vy = std::sin(angle) * speed;
            mote.size = kMoteSizeMin + (static_cast<double>(rand()) / RAND_MAX) * (kMoteSizeMax - kMoteSizeMin);
            mote.life_micros = 0.0;
            mote.max_life_micros = kMoteLifetimeMicros;
        } else {
            mote.x += mote.vx * dt;
            mote.y += mote.vy * dt;
        }
    }
}

// Phase 10: Render FX - edge glow, motes, joining flash (guide §32)
void RelictombsOverlay::Impl::render_fx(cairo_t* cr, double offset_x, double offset_y, double design_scale, double scale) {
    const guint64 now = g_get_monotonic_time();

    // 1. Joining flash during reconstruction
    if (state == RelictombsOverlay::Impl::State::Reconstructing && !repaired) {
        if (join_flash_start_micros == 0) {
            join_flash_start_micros = now;
        }
        double elapsed = static_cast<double>(now - join_flash_start_micros);
        double flash_progress = elapsed / kJoinFlashDurationMicros;
        if (flash_progress < 1.0) {
            // Soft gold-white glow at socket positions
            for (std::size_t i = 0; i < kFragmentSpecs.size(); ++i) {
                const auto& spec = kFragmentSpecs[i];
                double socket_x = offset_x + spec.socket_rect.x * design_scale * scale;
                double socket_y = offset_y + spec.socket_rect.y * design_scale * scale;
                double socket_w = spec.socket_rect.width * design_scale * scale;
                double socket_h = spec.socket_rect.height * design_scale * scale;
                double radius = std::max(socket_w, socket_h) * 0.6;

                cairo_save(cr);
                cairo_set_source_rgba(cr, 1.0, 0.85, 0.4, (1.0 - flash_progress) * 0.3);
                cairo_arc(cr, socket_x + socket_w * 0.5, socket_y + socket_h * 0.5, radius, 0, 2.0 * G_PI);
                cairo_fill(cr);
                cairo_restore(cr);
            }
        }
    } else {
        join_flash_start_micros = 0;
    }

    // 2. Faint warm edge glow at socket positions (during Reconstruction and Exit only, not Browsing)
    if (state == RelictombsOverlay::Impl::State::Reconstructing || state == RelictombsOverlay::Impl::State::Exiting) {
        for (std::size_t i = 0; i < kFragmentSpecs.size(); ++i) {
            const auto& spec = kFragmentSpecs[i];
            double socket_x = offset_x + spec.socket_rect.x * design_scale * scale;
            double socket_y = offset_y + spec.socket_rect.y * design_scale * scale;
            double socket_w = spec.socket_rect.width * design_scale * scale;
            double socket_h = spec.socket_rect.height * design_scale * scale;
            double radius = std::max(socket_w, socket_h) * 0.5;

            // Subtle pulsing glow
            double pulse = (std::sin(static_cast<double>(now) / 1'000'000.0 * 2.0) + 1.0) * 0.5; // 0 to 1
            double alpha = 0.05 + pulse * 0.08;

            cairo_save(cr);
            cairo_set_source_rgba(cr, 1.0, 0.9, 0.6, alpha);
            cairo_arc(cr, socket_x + socket_w * 0.5, socket_y + socket_h * 0.5, radius, 0, 2.0 * G_PI);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }

    // 3. Motes during Reconstruction and Exit only (not during Browsing)
    if (state == RelictombsOverlay::Impl::State::Reconstructing || state == RelictombsOverlay::Impl::State::Exiting) {
        update_motes(1.0 / 60.0); // approximate 60fps dt

        for (const auto& mote : motes) {
            double life_progress = mote.life_micros / mote.max_life_micros;
            double alpha = (1.0 - life_progress) * 0.6;
            double cx = offset_x + 960.0 * design_scale * scale + mote.x * design_scale * scale;
            double cy = offset_y + 540.0 * design_scale * scale + mote.y * design_scale * scale;

            cairo_save(cr);
            cairo_set_source_rgba(cr, 1.0, 0.95, 0.7, alpha);
            cairo_arc(cr, cx, cy, mote.size * design_scale * scale, 0, 2.0 * G_PI);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }
}

void RelictombsOverlay::Impl::request_apply() {
    if (state != RelictombsOverlay::Impl::State::Browsing) return;
    if (wallpaper_is_current()) {
        begin_apply();
        return;
    }
    // Selected wallpaper still decoding: remember the request; the decode
    // completion path starts the handshake the moment the swap lands.
    apply_requested = true;
}

bool RelictombsOverlay::Impl::handle_key(guint keyval) {
    std::cerr << "[Relictombs] handle_key: " << keyval << " state=" << static_cast<int>(state) << "\n";
    switch (keyval) {
    case GDK_KEY_Left:
    case GDK_KEY_h:
        navigate(-1);
        return true;
    case GDK_KEY_Right:
    case GDK_KEY_l:
        navigate(1);
        return true;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_space:
        request_apply();
        return true;
    case GDK_KEY_Escape:
        if (state == RelictombsOverlay::Impl::State::Browsing) {
            send(RelictombsResultKind::Cancel);
            hide();
            state = RelictombsOverlay::Impl::State::Closed;
            reset_fragments();
        }
        return true;
    case GDK_KEY_x:
    case GDK_KEY_X:
        // Phase 4 debug toggle: instantly flip between idle fragments and the
        // fully repaired arch. Lets socket geometry be tuned without waiting
        // on the reconstruction animation (guide §36 Phase 4).
        if (state == RelictombsOverlay::Impl::State::Browsing) {
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

    // Attach frame clock callback for Phase 5 idle floating animation (guide §11).
    // This runs at the monitor's refresh rate while the selector is visible.
    gtk_widget_add_tick_callback(canvas, &idle_animation_tick, impl_.get(), nullptr);

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
    impl_->state = RelictombsOverlay::Impl::State::Closed;
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
    if (impl_->state != RelictombsOverlay::Impl::State::Closed) {
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
    impl_->state = RelictombsOverlay::Impl::State::Browsing;
    impl_->last_navigation_micros = g_get_monotonic_time();
    impl_->reset_fragments();
    impl_->request_wallpaper_decode();
    impl_->present();
    std::cerr << "[Relictombs] session open: " << impl_->selected_path << '\n';
    return true;
}

void RelictombsOverlay::cancel() {
    if (impl_->state == RelictombsOverlay::Impl::State::Browsing) {
        impl_->send(RelictombsResultKind::Cancel);
        // Phase 9: Start exit animation on Esc (guide §24)
        impl_->start_exit_animation();
        impl_->reset_fragments();
    }
    // Applying/Reconstructing own their lifecycle; Closed is a no-op.
}

void RelictombsOverlay::backend_prepared() {
    if (impl_->state != RelictombsOverlay::Impl::State::Applying) return;
    impl_->send(RelictombsResultKind::Commit, impl_->selected_path);
}

void RelictombsOverlay::backend_committed() {
    if (impl_->state != RelictombsOverlay::Impl::State::Applying) return;
    impl_->send(RelictombsResultKind::Complete);
    // Phase 9: Start exit transition instead of immediately hiding (guide §22)
    impl_->start_exit_animation();
    // State will transition to Closed when exit animation completes
}

void RelictombsOverlay::backend_failed(std::string_view diagnostic) {
    if (impl_->state != RelictombsOverlay::Impl::State::Applying) return;

    // Guide §25: a commit failure must NOT close the selector. Return the
    // fragments to their broken idle positions, surface one short understated
    // diagnostic, and drop back to Browsing so the user can navigate or retry.
    // Never leave the arch permanently half-repaired.
    impl_->state = RelictombsOverlay::Impl::State::Browsing;
    impl_->repaired = false;
    impl_->reconstruct_started_micros = 0;
    impl_->reconstruct_progress = 0.0;
    impl_->apply_requested = false;
    impl_->error_message.assign(diagnostic);
    if (impl_->error_message.size() > kErrorLineMaxChars) {
        impl_->error_message.resize(kErrorLineMaxChars);
        impl_->error_message += "...";
    }
    impl_->queue_redraw();
    std::cerr << "[Relictombs] apply failed: " << diagnostic << '\n';
    // The shell restores the workspace on Cancel/Complete; a failed session
    // deliberately stays open, so no workspace restore is triggered here.
}

bool RelictombsOverlay::active() const noexcept {
    return impl_->state == RelictombsOverlay::Impl::State::Browsing ||
           impl_->state == RelictombsOverlay::Impl::State::Reconstructing ||
           impl_->state == RelictombsOverlay::Impl::State::Applying;
}

} // namespace realmheart::relictombs