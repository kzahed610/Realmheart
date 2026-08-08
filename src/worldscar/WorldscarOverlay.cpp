#include "worldscar/WorldscarOverlay.hpp"

#include "ui/LayerSurface.hpp"
#include "worldscar/WorldscarReferenceRenderer.hpp"

#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace realmheart::worldscar {
namespace {

constexpr double kOpeningDurationMs = 620.0;
constexpr double kNavigationDurationMs = 270.0;
constexpr double kCancelDurationMs = 250.0;
constexpr double kApplyDurationMs = 520.0;
constexpr double kFinishDurationMs = 170.0;
constexpr int kMaxQueuedNavigationSteps = 6;
constexpr double kSurfaceWidthFraction = 0.54;

void set_error(std::string* destination, std::string message) {
    if (destination != nullptr) *destination = std::move(message);
}

double unit_progress(gint64 started_us, gint64 now_us, double duration_ms) {
    if (started_us <= 0 || now_us <= started_us || duration_ms <= 0.0) {
        return 0.0;
    }
    return std::clamp(
        static_cast<double>(now_us - started_us) / (duration_ms * 1000.0),
        0.0,
        1.0
    );
}

double smoothstep(double value) {
    value = std::clamp(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
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

int navigation_sign(int direction) noexcept {
    return direction < 0 ? -1 : (direction > 0 ? 1 : 0);
}

int preferred_worldscar_width(GtkWidget* widget) noexcept {
    GdkMonitor* monitor = ui::resolve_layer_surface_monitor(widget);
    if (monitor == nullptr) return 1;
    GdkRectangle geometry{};
    gdk_monitor_get_geometry(monitor, &geometry);
    g_object_unref(monitor);

    return std::max(
        1,
        static_cast<int>(std::lround(
            static_cast<double>(geometry.width) * kSurfaceWidthFraction
        ))
    );
}

} // namespace

WorldscarOverlay::WorldscarOverlay(
    GtkApplication* application,
    CompletedCallback completed
) : completed_(std::move(completed)) {
    window_ = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(window_, "Realmheart Worldscar");
    gtk_window_set_decorated(window_, FALSE);
    gtk_window_set_resizable(window_, TRUE);
    // Worldscar only owns the left-side wound. A fullscreen GtkGLArea was
    // allocating several monitor-sized EGL buffers for transparent pixels and
    // dominated active PSS even after previews became thumbnails. Keep the
    // layer surface to the authored 54% screen region instead.
    gtk_window_set_default_size(
        window_, preferred_worldscar_width(GTK_WIDGET(window_)), 1
    );
    gtk_widget_add_css_class(GTK_WIDGET(window_), "realmheart-worldscar-window");
    gtk_widget_remove_css_class(GTK_WIDGET(window_), "background");

    ui::LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-worldscar";
    spec.layer = ui::LayerSurfaceLevel::Overlay;
    spec.keyboard_mode = ui::LayerKeyboardMode::Exclusive;
    spec.anchor_left = true;
    spec.anchor_right = false;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    spec.exclusive_zone = -1;
    ui::apply_layer_surface(window_, spec);
    gtk_layer_set_exclusive_zone(window_, -1);

    install_transparency_css();
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
    g_signal_connect(
        window_,
        "close-request",
        G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
            static_cast<WorldscarOverlay*>(data)->cancel();
            return TRUE;
        }),
        this
    );

    renderer_ = std::make_unique<WorldscarReferenceRenderer>();
    gtk_window_set_child(window_, renderer_->widget());

    key_controller_ = gtk_event_controller_key_new();
    g_signal_connect(
        key_controller_,
        "key-pressed",
        G_CALLBACK(&WorldscarOverlay::key_pressed_callback),
        this
    );
    g_object_ref(key_controller_);
    gtk_widget_add_controller(GTK_WIDGET(window_), key_controller_);
}

WorldscarOverlay::~WorldscarOverlay() {
    stop_tick();
    if (renderer_) renderer_->end_session();

    renderer_.reset();
    if (window_ != nullptr) {
        gtk_window_destroy(window_);
        window_ = nullptr;
    }
    if (key_controller_ != nullptr) {
        g_object_unref(key_controller_);
        key_controller_ = nullptr;
    }
    remove_transparency_css();
}

bool WorldscarOverlay::prepare(std::string* error) {
    if (renderer_ == nullptr) {
        set_error(error, "Worldscar renderer is unavailable");
        return false;
    }
    return renderer_->prepare(error);
}

bool WorldscarOverlay::preload_preview(
    const WorldscarSelection& selection,
    std::string* error
) {
    if (phase_ != Phase::Idle || renderer_ == nullptr) {
        set_error(error, "Worldscar cannot preload during an active session");
        return false;
    }
    return renderer_->preload_preview(selection.preview(), error);
}

void WorldscarOverlay::prewarm_thumbnail_cache(
    const std::vector<std::filesystem::path>& paths
) noexcept {
    if (renderer_ == nullptr || active()) return;
    renderer_->prewarm_thumbnail_cache(paths);
}

bool WorldscarOverlay::show(
    WorldscarSelection selection,
    std::string* error
) {
    if (phase_ != Phase::Idle || window_ == nullptr || renderer_ == nullptr) {
        set_error(error, "Worldscar overlay is already active");
        return false;
    }

    const auto preview = selection.preview();
    std::string renderer_error;
    if (!renderer_->begin(preview, &renderer_error)) {
        set_error(error, std::move(renderer_error));
        return false;
    }

    selection_ = std::move(selection);
    pending_selection_.reset();
    backend_failure_diagnostic_.clear();
    phase_ = Phase::WaitingFirstFrame;
    phase_started_us_ = 0;
    open_progress_ = 0.0;
    endpoint_frame_target_ = 0;
    pending_navigation_steps_ = 0;
    backend_prepared_ = false;

    gtk_window_present(window_);
    ensure_tick();
    if (error != nullptr) error->clear();
    return true;
}

void WorldscarOverlay::cancel() noexcept {
    if (phase_ == Phase::Idle || phase_ == Phase::Cancelling ||
        phase_ == Phase::WaitingCancelEndpoint ||
        phase_ == Phase::WaitingFailureEndpoint) {
        return;
    }

    if (phase_ == Phase::WaitingFirstFrame) {
        finish_session(
            WorldscarResult{WorldscarResultKind::Cancel, {}},
            true
        );
        return;
    }

    if (phase_ == Phase::Applying || phase_ == Phase::WaitingApplyEndpoint ||
        phase_ == Phase::AwaitingPrepared || phase_ == Phase::AwaitingBackend ||
        phase_ == Phase::Finishing || phase_ == Phase::WaitingFinishEndpoint) {
        return;
    }

    pending_navigation_steps_ = 0;
    cancel_start_open_ = open_progress_;
    phase_ = Phase::Cancelling;
    phase_started_us_ = 0;
    ensure_tick();
}

void WorldscarOverlay::backend_prepared() noexcept {
    if (phase_ == Phase::Idle) return;
    backend_prepared_ = true;
    if (phase_ == Phase::AwaitingPrepared) {
        request_backend_commit();
    }
}

void WorldscarOverlay::backend_committed() noexcept {
    if (phase_ != Phase::AwaitingBackend || renderer_ == nullptr) return;

    renderer_->set_finish_progress(0.0);
    phase_ = Phase::Finishing;
    phase_started_us_ = 0;
    ensure_tick();
}

void WorldscarOverlay::backend_failed(std::string diagnostic) noexcept {
    if (phase_ != Phase::Applying && phase_ != Phase::WaitingApplyEndpoint &&
        phase_ != Phase::AwaitingPrepared && phase_ != Phase::AwaitingBackend) {
        return;
    }
    if (renderer_ == nullptr) return;

    backend_failure_diagnostic_ = std::move(diagnostic);
    backend_prepared_ = false;
    renderer_->set_finish_progress(1.0);
    renderer_->set_commit_progress(0.0);
    renderer_->set_open_progress(0.0);
    open_progress_ = 0.0;
    endpoint_frame_target_ = renderer_->rendered_frames() + 1;
    phase_ = Phase::WaitingFailureEndpoint;
    ensure_tick();
}

void WorldscarOverlay::invalidate_candidate_cache() noexcept {
    if (phase_ != Phase::Idle || renderer_ == nullptr) return;
    renderer_->invalidate_candidate_cache();
}

bool WorldscarOverlay::active() const noexcept {
    return phase_ != Phase::Idle;
}

void WorldscarOverlay::request_navigation(int direction) noexcept {
    direction = navigation_sign(direction);
    if (direction == 0 || !selection_ || selection_->candidate_count() <= 1) {
        return;
    }
    if (phase_ != Phase::Browsing && phase_ != Phase::Navigating) return;

    if (phase_ == Phase::Browsing && start_navigation(direction)) return;

    pending_navigation_steps_ = std::clamp(
        pending_navigation_steps_ + direction,
        -kMaxQueuedNavigationSteps,
        kMaxQueuedNavigationSteps
    );
    ensure_tick();
}

bool WorldscarOverlay::start_navigation(int direction) noexcept {
    if (phase_ != Phase::Browsing || renderer_ == nullptr || !selection_) {
        return false;
    }

    WorldscarSelection future = *selection_;
    if (!future.navigate(direction)) return false;
    if (future.selected() == selection_->selected()) return false;
    if (!renderer_->can_navigate_to(future.selected())) return false;
    const auto future_preview = future.preview();
    if (!renderer_->preview_available_for_navigation(future_preview)) return false;

    int visual_direction = 0;
    std::string error;
    if (!renderer_->begin_navigation(
            future_preview,
            &visual_direction,
            &error)) {
        return false;
    }

    static_cast<void>(visual_direction);
    pending_selection_ = std::move(future);
    phase_ = Phase::Navigating;
    phase_started_us_ = 0;
    ensure_tick();
    return true;
}

void WorldscarOverlay::request_apply() noexcept {
    if (phase_ != Phase::Browsing || renderer_ == nullptr || !selection_ ||
        !renderer_->candidate_ready()) {
        return;
    }

    pending_navigation_steps_ = 0;
    backend_prepared_ = false;
    renderer_->set_open_progress(1.0);
    renderer_->set_commit_progress(0.0);
    renderer_->set_finish_progress(0.0);
    open_progress_ = 1.0;
    phase_ = Phase::Applying;
    phase_started_us_ = 0;

    // Start the expensive real-wallpaper preparation immediately. Worldscar
    // remains fully in control of visible pixels while the backend decodes and
    // uploads the selected full-resolution wallpaper invisibly underneath.
    if (completed_) {
        completed_(WorldscarResult{
            WorldscarResultKind::Apply,
            selection_->selected().string()
        });
    }
    ensure_tick();
}

void WorldscarOverlay::request_backend_commit() noexcept {
    if (!selection_ || !backend_prepared_ ||
        (phase_ != Phase::AwaitingPrepared &&
         phase_ != Phase::WaitingApplyEndpoint)) {
        return;
    }

    phase_ = Phase::AwaitingBackend;
    phase_started_us_ = 0;
    if (completed_) {
        completed_(WorldscarResult{
            WorldscarResultKind::Commit,
            selection_->selected().string()
        });
    }
}

void WorldscarOverlay::finish_session(
    WorldscarResult result,
    bool notify
) noexcept {
    stop_tick();

    if (window_ != nullptr) {
        gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
    }
    if (renderer_ != nullptr) renderer_->end_session();

    phase_ = Phase::Idle;
    phase_started_us_ = 0;
    open_progress_ = 0.0;
    cancel_start_open_ = 0.0;
    endpoint_frame_target_ = 0;
    pending_navigation_steps_ = 0;
    backend_prepared_ = false;
    selection_.reset();
    pending_selection_.reset();

    if (notify && completed_) completed_(std::move(result));
}

void WorldscarOverlay::ensure_tick() noexcept {
    if (tick_id_ != 0 || window_ == nullptr) return;
    tick_id_ = gtk_widget_add_tick_callback(
        GTK_WIDGET(window_),
        &WorldscarOverlay::tick_callback,
        this,
        nullptr
    );
}

void WorldscarOverlay::stop_tick() noexcept {
    if (tick_id_ == 0 || window_ == nullptr) return;
    gtk_widget_remove_tick_callback(GTK_WIDGET(window_), tick_id_);
    tick_id_ = 0;
}

void WorldscarOverlay::install_transparency_css() {
    GdkDisplay* display = gdk_display_get_default();
    if (display == nullptr) return;

    transparency_provider_ = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        transparency_provider_,
        R"CSS(
            window.realmheart-worldscar-window,
            window.realmheart-worldscar-window > *,
            .realmheart-worldscar-gl {
                background: transparent;
                background-color: rgba(0, 0, 0, 0);
                background-image: none;
                border: none;
                box-shadow: none;
            }
        )CSS"
    );
    gtk_style_context_add_provider_for_display(
        display,
        GTK_STYLE_PROVIDER(transparency_provider_),
        GTK_STYLE_PROVIDER_PRIORITY_USER + 1
    );
}

void WorldscarOverlay::remove_transparency_css() noexcept {
    if (transparency_provider_ == nullptr) return;
    if (GdkDisplay* display = gdk_display_get_default(); display != nullptr) {
        gtk_style_context_remove_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(transparency_provider_)
        );
    }
    g_clear_object(&transparency_provider_);
}

gboolean WorldscarOverlay::tick_callback(
    GtkWidget*,
    GdkFrameClock* frame_clock,
    gpointer data
) {
    auto* self = static_cast<WorldscarOverlay*>(data);
    return self != nullptr ? self->tick(frame_clock) : G_SOURCE_REMOVE;
}

gboolean WorldscarOverlay::key_pressed_callback(
    GtkEventControllerKey*,
    guint keyval,
    guint,
    GdkModifierType,
    gpointer data
) {
    auto* self = static_cast<WorldscarOverlay*>(data);
    if (self == nullptr) return FALSE;

    if (keyval == GDK_KEY_Escape) {
        self->cancel();
        return TRUE;
    }
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up ||
        keyval == GDK_KEY_k || keyval == GDK_KEY_K) {
        self->request_navigation(-1);
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down ||
        keyval == GDK_KEY_j || keyval == GDK_KEY_J) {
        self->request_navigation(1);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        self->request_apply();
        return TRUE;
    }
    return FALSE;
}

gboolean WorldscarOverlay::tick(GdkFrameClock* frame_clock) noexcept {
    if (phase_ == Phase::Idle) {
        tick_id_ = 0;
        return G_SOURCE_REMOVE;
    }
    if (renderer_ == nullptr) {
        tick_id_ = 0;
        finish_session(
            WorldscarResult{
                WorldscarResultKind::Error,
                "Worldscar renderer disappeared during session"
            },
            true
        );
        return G_SOURCE_REMOVE;
    }

    renderer_->poll_async();
    if (renderer_->failed()) {
        const std::string diagnostic = renderer_->failure_message();
        tick_id_ = 0;
        finish_session(
            WorldscarResult{WorldscarResultKind::Error, diagnostic},
            true
        );
        return G_SOURCE_REMOVE;
    }

    const gint64 now_us = gdk_frame_clock_get_frame_time(frame_clock);
    switch (phase_) {
    case Phase::Idle:
        tick_id_ = 0;
        return G_SOURCE_REMOVE;

    case Phase::WaitingFirstFrame:
        if (!renderer_->frame_ready() || !renderer_->preview_ready()) {
            return G_SOURCE_CONTINUE;
        }
        renderer_->reveal();
        phase_ = Phase::Opening;
        phase_started_us_ = now_us;
        open_progress_ = 0.0;
        return G_SOURCE_CONTINUE;

    case Phase::Opening: {
        const double progress = unit_progress(
            phase_started_us_, now_us, kOpeningDurationMs
        );
        open_progress_ = smoothstep(progress);
        renderer_->set_open_progress(open_progress_);
        if (progress >= 1.0) {
            open_progress_ = 1.0;
            renderer_->set_open_progress(1.0);
            phase_ = Phase::Browsing;
            phase_started_us_ = 0;
        }
        return G_SOURCE_CONTINUE;
    }

    case Phase::Browsing: {
        if (pending_navigation_steps_ != 0) {
            const int direction = navigation_sign(pending_navigation_steps_);
            if (start_navigation(direction)) {
                pending_navigation_steps_ -= direction;
                return G_SOURCE_CONTINUE;
            }

            // Keep the queued direction alive while the invisible one-step
            // lookahead finishes. This is bounded input, not a busy decode loop;
            // the tick disappears as soon as the requested preview can morph.
        }

        if (renderer_->preview_ready() && pending_navigation_steps_ == 0) {
            tick_id_ = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    case Phase::Navigating: {
        if (phase_started_us_ == 0) phase_started_us_ = now_us;
        const double progress = unit_progress(
            phase_started_us_, now_us, kNavigationDurationMs
        );
        renderer_->set_navigation_progress(smoothstep(progress));
        if (progress >= 1.0 && pending_selection_) {
            renderer_->set_navigation_progress(1.0);
            selection_ = std::move(pending_selection_);
            pending_selection_.reset();
            renderer_->complete_navigation(selection_->preview());
            phase_ = Phase::Browsing;
            phase_started_us_ = 0;
        }
        return G_SOURCE_CONTINUE;
    }

    case Phase::Cancelling: {
        if (phase_started_us_ == 0) phase_started_us_ = now_us;
        const double progress = unit_progress(
            phase_started_us_, now_us, kCancelDurationMs
        );
        open_progress_ = cancel_start_open_ * (1.0 - smoothstep(progress));
        renderer_->set_commit_progress(0.0);
        renderer_->set_open_progress(open_progress_);
        if (progress >= 1.0) {
            open_progress_ = 0.0;
            renderer_->set_open_progress(0.0);
            renderer_->set_commit_progress(0.0);
            endpoint_frame_target_ = renderer_->rendered_frames() + 1;
            phase_ = Phase::WaitingCancelEndpoint;
        }
        return G_SOURCE_CONTINUE;
    }

    case Phase::Applying: {
        if (phase_started_us_ == 0) phase_started_us_ = now_us;
        const double progress = unit_progress(
            phase_started_us_, now_us, kApplyDurationMs
        );
        renderer_->set_commit_progress(smoothstep(progress));
        if (progress >= 1.0) {
            renderer_->set_commit_progress(1.0);
            endpoint_frame_target_ = renderer_->rendered_frames() + 1;
            phase_ = Phase::WaitingApplyEndpoint;
        }
        return G_SOURCE_CONTINUE;
    }

    case Phase::WaitingCancelEndpoint:
        if (renderer_->rendered_frames() >= endpoint_frame_target_) {
            tick_id_ = 0;
            finish_session(
                WorldscarResult{WorldscarResultKind::Cancel, {}},
                true
            );
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;

    case Phase::WaitingApplyEndpoint:
        if (renderer_->rendered_frames() >= endpoint_frame_target_) {
            if (backend_prepared_) {
                tick_id_ = 0;
                request_backend_commit();
                return G_SOURCE_REMOVE;
            }
            phase_ = Phase::AwaitingPrepared;
            tick_id_ = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;

    case Phase::AwaitingPrepared:
    case Phase::AwaitingBackend:
        tick_id_ = 0;
        return G_SOURCE_REMOVE;

    case Phase::Finishing: {
        if (phase_started_us_ == 0) phase_started_us_ = now_us;
        const double progress = unit_progress(
            phase_started_us_, now_us, kFinishDurationMs
        );
        renderer_->set_finish_progress(smoothstep(progress));
        if (progress >= 1.0) {
            renderer_->set_finish_progress(1.0);
            endpoint_frame_target_ = renderer_->rendered_frames() + 1;
            phase_ = Phase::WaitingFinishEndpoint;
        }
        return G_SOURCE_CONTINUE;
    }

    case Phase::WaitingFinishEndpoint:
        if (renderer_->rendered_frames() >= endpoint_frame_target_) {
            tick_id_ = 0;
            finish_session(
                WorldscarResult{WorldscarResultKind::Complete, {}},
                true
            );
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;

    case Phase::WaitingFailureEndpoint:
        if (renderer_->rendered_frames() >= endpoint_frame_target_) {
            const std::string diagnostic = backend_failure_diagnostic_.empty()
                ? "wallpaper backend rejected Worldscar selection"
                : backend_failure_diagnostic_;
            tick_id_ = 0;
            finish_session(
                WorldscarResult{WorldscarResultKind::Error, diagnostic},
                true
            );
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    return G_SOURCE_CONTINUE;
}

} // namespace realmheart::worldscar
