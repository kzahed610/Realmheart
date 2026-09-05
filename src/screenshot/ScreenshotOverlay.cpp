#include "screenshot/ScreenshotOverlay.hpp"

#include "screenshot/ClipboardExporter.hpp"
#include "screenshot/OcrEngine.hpp"
#include "screenshot/SelectionGeometry.hpp"
#include "screenshot/SmartRegionLoader.hpp"
#include "ui/LayerSurface.hpp"

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace realmheart::screenshot {
namespace {

constexpr double kDimAlpha = 0.44;
constexpr double kSmartTargetDimAlpha = 0.18;
constexpr double kSemanticOutlineAlpha = 0.20;
constexpr double kGuideAlpha = 0.60;
constexpr double kMinimumSelectionLogical = 3.0;
constexpr double kPi = 3.14159265358979323846;
constexpr auto kHintInitialHold = std::chrono::milliseconds(2400);
constexpr auto kHintInteractionHold = std::chrono::milliseconds(1150);
constexpr auto kHintFadeDuration = std::chrono::milliseconds(340);
constexpr auto kSmartEmphasisDuration = std::chrono::milliseconds(170);
constexpr auto kEntrySweepDuration = std::chrono::milliseconds(520);
constexpr auto kOcrSweepDuration = std::chrono::milliseconds(860);
constexpr auto kSemanticStructureFadeDuration = std::chrono::milliseconds(440);
constexpr auto kSemanticContentFadeDelay = std::chrono::milliseconds(100);
constexpr auto kSemanticContentFadeDuration = std::chrono::milliseconds(260);
constexpr auto kSemanticRevealTotalDuration = std::chrono::milliseconds(440);
constexpr auto kErrorHintHold = std::chrono::milliseconds(2600);

struct Rgba {
    double r = 1.0;
    double g = 1.0;
    double b = 1.0;
    double a = 1.0;
};

constexpr Rgba kAetherGold{0.96, 0.84, 0.42, 1.0};
constexpr Rgba kAetherGoldSoft{0.92, 0.77, 0.35, 1.0};
constexpr Rgba kArcaneViolet{0.62, 0.47, 0.96, 1.0};
constexpr Rgba kArcaneVioletSoft{0.54, 0.42, 0.90, 1.0};
constexpr Rgba kLayerNeutral{0.76, 0.78, 0.90, 1.0};
constexpr Rgba kTextWhite{0.98, 0.98, 1.0, 1.0};
constexpr Rgba kBackdrop{0.05, 0.05, 0.07, 0.86};
constexpr Rgba kBackdropBorder{1.0, 1.0, 1.0, 0.08};

void set_source_rgba(cairo_t* cr, const Rgba& color, double alpha_scale = 1.0) {
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a * alpha_scale);
}

Rgba alpha(const Rgba& color, double a) {
    return Rgba{color.r, color.g, color.b, a};
}

Rgba smart_target_color(SemanticRegionSource source) {
    switch (source) {
        case SemanticRegionSource::Content:
            return kAetherGoldSoft;
        case SemanticRegionSource::Layer:
            return kLayerNeutral;
        case SemanticRegionSource::Window:
        default:
            return kArcaneVioletSoft;
    }
}

struct DetectionState {
    std::mutex mutex;
    std::optional<SmartRegionLoadResult> result;
    std::atomic_bool ready{false};
    std::atomic_bool cancel_requested{false};
};

struct OcrState {
    std::mutex mutex;
    std::optional<OcrResult> result;
    std::atomic_bool ready{false};
    std::atomic_bool cancel_requested{false};
};

struct OverlayContext {
    const FrozenFrame* frame = nullptr;
    MonitorTarget monitor;
    SemanticRegionSnapshot semantic_storage;
    const SemanticRegionSnapshot* semantic_regions = &semantic_storage;
    std::chrono::steady_clock::time_point process_start{};
    GtkWindow* window = nullptr;
    GtkWidget* canvas = nullptr;

    DetectionState detection_state;
    std::thread detection_thread;
    guint detection_poll_source = 0;
    bool detection_started = false;

    OcrState ocr_state;
    std::thread ocr_thread;
    guint ocr_poll_source = 0;

    bool pointer_inside = false;
    bool selecting = false;
    bool completed = false;
    bool shutting_down = false;
    double pointer_x = 0.0;
    double pointer_y = 0.0;
    double start_x = 0.0;
    double start_y = 0.0;
    double current_x = 0.0;
    double current_y = 0.0;

    SelectionRatio ratio = SelectionRatio::Free;
    std::array<GtkToggleButton*, 4> ratio_buttons{};
    GtkWidget* ratio_toolbar = nullptr;

    guint visual_tick_source = 0;
    std::chrono::steady_clock::time_point visual_tick_until{};
    std::chrono::steady_clock::time_point hint_full_until{};
    std::chrono::steady_clock::time_point hint_fade_until{};
    std::string transient_error;
    std::chrono::steady_clock::time_point transient_error_until{};
    const SemanticRegion* visual_smart_region = nullptr;
    std::size_t visual_smart_candidate = 0;
    std::chrono::steady_clock::time_point smart_emphasis_started{};
    bool entry_sweep_active = false;
    bool entry_sweep_pending_first_draw = false;
    std::chrono::steady_clock::time_point entry_sweep_started{};
    bool ocr_sweep_active = false;
    bool ocr_sweep_pending_first_draw = false;
    std::chrono::steady_clock::time_point ocr_sweep_started{};
    bool semantic_reveal_active = false;
    bool semantic_reveal_pending_first_draw = false;
    std::chrono::steady_clock::time_point semantic_reveal_started{};

    bool ocr_region_selecting = false;
    bool ocr_processing = false;
    bool ocr_revealing = false;
    bool ocr_ready = false;
    bool ocr_text_selecting = false;
    SelectionRect ocr_region_canvas{};
    PixelRect ocr_region_pixels{};
    std::vector<OcrWord> pending_ocr_words;
    std::vector<OcrWord> ocr_words;
    std::optional<std::size_t> ocr_anchor_word;
    std::optional<std::size_t> ocr_focus_word;

    std::size_t smart_target_cycle = 0;
    double smart_cycle_anchor_x = 0.0;
    double smart_cycle_anchor_y = 0.0;
    const SemanticRegion* smart_cycle_anchor_primary = nullptr;
    bool smart_cycle_anchor_valid = false;
};

void refresh_hint(
    OverlayContext* context,
    std::chrono::milliseconds hold = kHintInteractionHold
);
void update_interaction_cursor(OverlayContext* context);
void start_semantic_reveal(OverlayContext* context);
void show_transient_error(OverlayContext* context, std::string message);
void request_async_cancellation(OverlayContext* context);
void remove_async_sources(OverlayContext* context);
bool reap_finished_ocr_worker(OverlayContext* context);
void rounded_rectangle(
    cairo_t* cr,
    double x,
    double y,
    double width,
    double height,
    double radius
);

GdkMonitor* find_monitor_by_connector(GtkWidget* widget, const std::string& connector) {
    if (widget == nullptr || connector.empty()) return nullptr;

    GdkDisplay* display = gtk_widget_get_display(widget);
    if (display == nullptr) return nullptr;

    GListModel* monitors = gdk_display_get_monitors(display);
    if (monitors == nullptr) return nullptr;

    const guint count = g_list_model_get_n_items(monitors);
    for (guint index = 0; index < count; ++index) {
        GdkMonitor* monitor = GDK_MONITOR(g_list_model_get_item(monitors, index));
        if (monitor == nullptr) continue;

        const char* current_connector = gdk_monitor_get_connector(monitor);
        if (current_connector != nullptr && connector == current_connector) {
            return monitor; // referenced by g_list_model_get_item; caller owns ref
        }
        g_object_unref(monitor);
    }
    return nullptr;
}

double clamp_coordinate(double value, int extent) {
    return std::clamp(value, 0.0, static_cast<double>(std::max(0, extent)));
}

void queue_canvas(OverlayContext* context) {
    if (context != nullptr && !context->shutting_down && context->canvas != nullptr) {
        gtk_widget_queue_draw(context->canvas);
    }
}

gboolean on_visual_tick(GtkWidget*, GdkFrameClock*, gpointer user_data) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr) return G_SOURCE_REMOVE;
    if (context->shutting_down) {
        context->visual_tick_source = 0;
        return G_SOURCE_REMOVE;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool hint_changing =
        now >= context->hint_full_until && now <= context->hint_fade_until;
    const bool smart_changing =
        context->visual_smart_region != nullptr &&
        now <= context->smart_emphasis_started + kSmartEmphasisDuration;
    const bool entry_changing =
        context->entry_sweep_active &&
        (context->entry_sweep_pending_first_draw ||
         now <= context->entry_sweep_started + kEntrySweepDuration);
    const bool ocr_changing =
        context->ocr_sweep_active &&
        (context->ocr_sweep_pending_first_draw ||
         now <= context->ocr_sweep_started + kOcrSweepDuration);
    const bool semantic_changing =
        context->semantic_reveal_active &&
        (context->semantic_reveal_pending_first_draw ||
         now <= context->semantic_reveal_started + kSemanticRevealTotalDuration);

    if (!entry_changing) context->entry_sweep_active = false;
    if (!ocr_changing) {
        context->ocr_sweep_active = false;
        if (context->ocr_revealing) {
            context->ocr_words = std::move(context->pending_ocr_words);
            context->pending_ocr_words.clear();
            context->ocr_revealing = false;
            context->ocr_ready = true;
            context->ocr_anchor_word.reset();
            context->ocr_focus_word.reset();
            update_interaction_cursor(context);
            refresh_hint(context);
        }
    }

    if (!semantic_changing) context->semantic_reveal_active = false;

    if (hint_changing || smart_changing || entry_changing || ocr_changing ||
        semantic_changing || now >= context->visual_tick_until) {
        queue_canvas(context);
    }
    if (now >= context->visual_tick_until &&
        !hint_changing && !smart_changing && !entry_changing && !ocr_changing &&
        !semantic_changing) {
        context->visual_tick_source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void animate_until(
    OverlayContext* context,
    std::chrono::steady_clock::time_point deadline
) {
    if (context == nullptr || context->shutting_down) return;
    context->visual_tick_until = std::max(context->visual_tick_until, deadline);
    if (context->visual_tick_source == 0 && context->canvas != nullptr) {
        context->visual_tick_source = gtk_widget_add_tick_callback(
            context->canvas,
            on_visual_tick,
            context,
            nullptr
        );
    }
}

void show_transient_error(OverlayContext* context, std::string message) {
    if (context == nullptr || context->shutting_down || message.empty()) return;
    for (char& ch : message) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    constexpr std::size_t kMaxErrorLength = 110;
    if (message.size() > kMaxErrorLength) {
        message.resize(kMaxErrorLength - 3);
        message += "...";
    }
    const auto now = std::chrono::steady_clock::now();
    context->transient_error = std::move(message);
    context->transient_error_until = now + kErrorHintHold;
    context->hint_full_until = context->transient_error_until;
    context->hint_fade_until = context->hint_full_until + kHintFadeDuration;
    animate_until(context, context->hint_fade_until);
    queue_canvas(context);
}

void refresh_hint(OverlayContext* context, std::chrono::milliseconds hold) {
    if (context == nullptr || context->shutting_down) return;
    const auto now = std::chrono::steady_clock::now();
    context->hint_full_until = now + hold;
    context->hint_fade_until = context->hint_full_until + kHintFadeDuration;
    animate_until(context, context->hint_fade_until);
    queue_canvas(context);
}

void request_async_cancellation(OverlayContext* context) {
    if (context == nullptr) return;
    context->detection_state.cancel_requested.store(true, std::memory_order_release);
    context->ocr_state.cancel_requested.store(true, std::memory_order_release);
}

void remove_async_sources(OverlayContext* context) {
    if (context == nullptr) return;
    if (context->visual_tick_source != 0 && context->canvas != nullptr) {
        gtk_widget_remove_tick_callback(context->canvas, context->visual_tick_source);
        context->visual_tick_source = 0;
    }
    if (context->detection_poll_source != 0) {
        g_source_remove(context->detection_poll_source);
        context->detection_poll_source = 0;
    }
    if (context->ocr_poll_source != 0) {
        g_source_remove(context->ocr_poll_source);
        context->ocr_poll_source = 0;
    }
}

bool reap_finished_ocr_worker(OverlayContext* context) {
    if (context == nullptr || !context->ocr_thread.joinable()) return true;
    if (!context->ocr_state.ready.load(std::memory_order_acquire)) return false;

    if (context->ocr_poll_source != 0) {
        g_source_remove(context->ocr_poll_source);
        context->ocr_poll_source = 0;
    }
    context->ocr_thread.join();
    {
        std::lock_guard lock(context->ocr_state.mutex);
        context->ocr_state.result.reset();
    }
    context->ocr_state.ready.store(false, std::memory_order_release);
    context->ocr_state.cancel_requested.store(false, std::memory_order_release);
    return true;
}

double hint_opacity(const OverlayContext& context) {
    const auto now = std::chrono::steady_clock::now();
    if (now <= context.hint_full_until) return 1.0;
    if (now >= context.hint_fade_until) return 0.34;

    const auto fade_total = std::chrono::duration<double>(
        context.hint_fade_until - context.hint_full_until
    ).count();
    if (fade_total <= 0.0) return 0.34;
    const auto fade_elapsed = std::chrono::duration<double>(
        now - context.hint_full_until
    ).count();
    const double t = std::clamp(fade_elapsed / fade_total, 0.0, 1.0);
    return 1.0 - t * 0.66;
}

double ease_out_cubic(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return 1.0 - std::pow(1.0 - t, 3.0);
}

double ease_in_out(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double animation_progress(
    bool active,
    std::chrono::steady_clock::time_point started,
    std::chrono::milliseconds duration
) {
    if (!active) return 1.0;
    const auto total = std::chrono::duration<double>(duration).count();
    if (total <= 0.0) return 1.0;
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started
    ).count();
    return std::clamp(elapsed / total, 0.0, 1.0);
}

double semantic_reveal_progress(
    const OverlayContext& context,
    SemanticRegionSource source
) {
    if (!context.semantic_reveal_active) return 1.0;

    if (source == SemanticRegionSource::Content) {
        const auto content_started = context.semantic_reveal_started +
            kSemanticContentFadeDelay;
        if (std::chrono::steady_clock::now() <= content_started) return 0.0;
        return ease_out_cubic(animation_progress(
            true,
            content_started,
            kSemanticContentFadeDuration
        ));
    }

    return ease_out_cubic(animation_progress(
        true,
        context.semantic_reveal_started,
        kSemanticStructureFadeDuration
    ));
}

double semantic_resolve_pulse(double reveal) {
    const double t = std::clamp(reveal, 0.0, 1.0);
    return std::sin(t * kPi) * (1.0 - 0.22 * t);
}

void draw_semantic_corner_trace(
    cairo_t* cr,
    const SelectionRect& outline,
    const Rgba& accent,
    double reveal,
    double alpha_scale
) {
    const double x = outline.x + 0.5;
    const double y = outline.y + 0.5;
    const double width = std::max(0.0, outline.width - 1.0);
    const double height = std::max(0.0, outline.height - 1.0);
    if (width <= 0.0 || height <= 0.0) return;

    const double right = x + width;
    const double bottom = y + height;
    const double radius = 8.0;
    const double max_corner = std::max(8.0, std::min(width, height) * 0.24);
    const double trace = std::clamp(reveal, 0.0, 1.0);
    const double corner_len = std::max(6.0, max_corner * (0.30 + 0.70 * trace));

    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    cairo_set_line_width(cr, 3.4);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.24 * alpha_scale);
    cairo_move_to(cr, x + radius * 0.7, y);
    cairo_line_to(cr, std::min(right - radius, x + corner_len), y);
    cairo_move_to(cr, x, y + radius * 0.7);
    cairo_line_to(cr, x, std::min(bottom - radius, y + corner_len));
    cairo_move_to(cr, std::max(x + radius, right - corner_len), y);
    cairo_line_to(cr, right - radius * 0.7, y);
    cairo_move_to(cr, right, y + radius * 0.7);
    cairo_line_to(cr, right, std::min(bottom - radius, y + corner_len));
    cairo_move_to(cr, x + radius * 0.7, bottom);
    cairo_line_to(cr, std::min(right - radius, x + corner_len), bottom);
    cairo_move_to(cr, x, std::max(y + radius, bottom - corner_len));
    cairo_line_to(cr, x, bottom - radius * 0.7);
    cairo_move_to(cr, std::max(x + radius, right - corner_len), bottom);
    cairo_line_to(cr, right - radius * 0.7, bottom);
    cairo_move_to(cr, right, std::max(y + radius, bottom - corner_len));
    cairo_line_to(cr, right, bottom - radius * 0.7);
    cairo_stroke(cr);

    cairo_set_line_width(cr, 1.8);
    set_source_rgba(cr, accent, alpha_scale);
    cairo_move_to(cr, x + radius * 0.7, y);
    cairo_line_to(cr, std::min(right - radius, x + corner_len), y);
    cairo_move_to(cr, x, y + radius * 0.7);
    cairo_line_to(cr, x, std::min(bottom - radius, y + corner_len));
    cairo_move_to(cr, std::max(x + radius, right - corner_len), y);
    cairo_line_to(cr, right - radius * 0.7, y);
    cairo_move_to(cr, right, y + radius * 0.7);
    cairo_line_to(cr, right, std::min(bottom - radius, y + corner_len));
    cairo_move_to(cr, x + radius * 0.7, bottom);
    cairo_line_to(cr, std::min(right - radius, x + corner_len), bottom);
    cairo_move_to(cr, x, std::max(y + radius, bottom - corner_len));
    cairo_line_to(cr, x, bottom - radius * 0.7);
    cairo_move_to(cr, std::max(x + radius, right - corner_len), bottom);
    cairo_line_to(cr, right - radius * 0.7, bottom);
    cairo_move_to(cr, right, std::max(y + radius, bottom - corner_len));
    cairo_line_to(cr, right, bottom - radius * 0.7);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void draw_semantic_resolve_fx(
    cairo_t* cr,
    const SelectionRect& outline,
    const Rgba& accent,
    double reveal,
    double base_alpha,
    double fill_alpha
) {
    const double pulse = semantic_resolve_pulse(reveal);
    const double x = outline.x;
    const double y = outline.y;
    const double width = outline.width;
    const double height = outline.height;
    if (width <= 1.0 || height <= 1.0) return;

    cairo_save(cr);

    const double bloom_expand = 10.0 * (1.0 - reveal) + 2.0;
    rounded_rectangle(
        cr,
        x - bloom_expand,
        y - bloom_expand,
        width + bloom_expand * 2.0,
        height + bloom_expand * 2.0,
        11.0 + bloom_expand * 0.55
    );
    set_source_rgba(cr, accent, pulse * 0.085);
    cairo_fill(cr);

    rounded_rectangle(cr, x, y, width, height, 8.0);
    set_source_rgba(cr, accent, fill_alpha * (0.40 + 0.60 * reveal) + pulse * 0.032);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    set_source_rgba(cr, accent, base_alpha * (0.22 + 0.78 * reveal));
    cairo_stroke(cr);

    draw_semantic_corner_trace(
        cr,
        outline,
        accent,
        reveal,
        0.48 + 0.34 * pulse + 0.18 * reveal
    );

    cairo_restore(cr);
}

void start_entry_sweep(OverlayContext* context) {
    if (context == nullptr) return;
    context->entry_sweep_active = true;
    context->entry_sweep_pending_first_draw = true;
    context->entry_sweep_started = {};
    animate_until(context, std::chrono::steady_clock::now() + kEntrySweepDuration);
    queue_canvas(context);
}

void start_ocr_sweep(OverlayContext* context) {
    if (context == nullptr) return;
    context->ocr_sweep_active = true;
    context->ocr_sweep_pending_first_draw = true;
    context->ocr_sweep_started = {};
    animate_until(context, std::chrono::steady_clock::now() + kOcrSweepDuration);
    queue_canvas(context);
}

void start_semantic_reveal(OverlayContext* context) {
    if (context == nullptr || context->semantic_regions == nullptr ||
        !context->semantic_regions->available ||
        context->semantic_regions->regions.empty()) {
        return;
    }
    context->semantic_reveal_active = true;
    context->semantic_reveal_pending_first_draw = true;
    context->semantic_reveal_started = {};
    animate_until(
        context,
        std::chrono::steady_clock::now() + kSemanticRevealTotalDuration
    );
    queue_canvas(context);
}

void update_interaction_cursor(OverlayContext* context) {
    if (context == nullptr || context->shutting_down || context->canvas == nullptr) return;

    // The compositor cursor is rendered independently from our Cairo overlay.
    // Keeping a native crosshair visible beside application-rendered guide lines
    // makes the one-frame difference look like pointer lag. Hide the native
    // cursor during capture so the guide intersection is the single visual
    // pointer. OCR keeps native wait/text cursors where full-screen guides are
    // not used as the interaction affordance.
    const char* cursor = "none";
    if (context->ocr_processing || context->ocr_revealing) {
        cursor = "wait";
    } else if (context->ocr_ready) {
        cursor = "text";
    }
    gtk_widget_set_cursor_from_name(context->canvas, cursor);
}

SelectionRect current_selection(const OverlayContext& context) {
    if (context.canvas == nullptr) return {};

    return selection_for_ratio(
        context.start_x,
        context.start_y,
        context.current_x,
        context.current_y,
        gtk_widget_get_width(context.canvas),
        gtk_widget_get_height(context.canvas),
        context.ratio
    );
}

struct SmartTargetView {
    const SemanticRegion* region = nullptr;
    SelectionRect rect;
    std::size_t candidate_index = 0;
    std::size_t candidate_count = 0;
};

SelectionRect semantic_rect_to_canvas(
    const SemanticRegionSnapshot& snapshot,
    const SelectionRect& rect,
    int canvas_width,
    int canvas_height
) {
    if (snapshot.monitor_width <= 0.0 || snapshot.monitor_height <= 0.0 ||
        canvas_width <= 0 || canvas_height <= 0 ||
        !std::isfinite(rect.x) || !std::isfinite(rect.y) ||
        !std::isfinite(rect.width) || !std::isfinite(rect.height) ||
        rect.width <= 0.0 || rect.height <= 0.0) {
        return {};
    }

    const double scale_x = static_cast<double>(canvas_width) / snapshot.monitor_width;
    const double scale_y = static_cast<double>(canvas_height) / snapshot.monitor_height;
    const double left = std::clamp(rect.x * scale_x, 0.0, static_cast<double>(canvas_width));
    const double top = std::clamp(rect.y * scale_y, 0.0, static_cast<double>(canvas_height));
    const double right = std::clamp(
        (rect.x + rect.width) * scale_x,
        0.0,
        static_cast<double>(canvas_width)
    );
    const double bottom = std::clamp(
        (rect.y + rect.height) * scale_y,
        0.0,
        static_cast<double>(canvas_height)
    );
    if (right <= left || bottom <= top) return {};

    return SelectionRect{
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };
}

bool contains_point(const SelectionRect& rect, double x, double y) {
    return rect.width > 1.0 && rect.height > 1.0 &&
        x >= rect.x && y >= rect.y &&
        x < rect.x + rect.width && y < rect.y + rect.height;
}

double rect_intersection_area(const SelectionRect& left, const SelectionRect& right) {
    const double x1 = std::max(left.x, right.x);
    const double y1 = std::max(left.y, right.y);
    const double x2 = std::min(left.x + left.width, right.x + right.width);
    const double y2 = std::min(left.y + left.height, right.y + right.height);
    return std::max(0.0, x2 - x1) * std::max(0.0, y2 - y1);
}

double rect_iou(const SelectionRect& left, const SelectionRect& right) {
    const double intersection = rect_intersection_area(left, right);
    if (intersection <= 0.0) return 0.0;
    const double left_area = left.width * left.height;
    const double right_area = right.width * right.height;
    const double union_area = left_area + right_area - intersection;
    return union_area > 0.0 ? intersection / union_area : 0.0;
}

bool visually_same_target(const SelectionRect& left, const SelectionRect& right) {
    constexpr double edge_epsilon = 4.0;
    const bool edges_match =
        std::abs(left.x - right.x) <= edge_epsilon &&
        std::abs(left.y - right.y) <= edge_epsilon &&
        std::abs((left.x + left.width) - (right.x + right.width)) <= edge_epsilon &&
        std::abs((left.y + left.height) - (right.y + right.height)) <= edge_epsilon;
    return edges_match || rect_iou(left, right) >= 0.965;
}

std::vector<SmartTargetView> smart_targets_at(
    const OverlayContext& context,
    double x,
    double y
) {
    std::vector<SmartTargetView> targets;
    if (context.canvas == nullptr || context.semantic_regions == nullptr ||
        !context.semantic_regions->available) {
        return targets;
    }

    const int width = gtk_widget_get_width(context.canvas);
    const int height = gtk_widget_get_height(context.canvas);
    for (const auto& region : context.semantic_regions->regions) {
        const SelectionRect rect = semantic_rect_to_canvas(
            *context.semantic_regions,
            region.rect,
            width,
            height
        );
        if (!contains_point(rect, x, y)) continue;

        bool duplicate = false;
        for (const auto& existing : targets) {
            if (visually_same_target(existing.rect, rect)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        targets.push_back(SmartTargetView{
            .region = &region,
            .rect = rect,
        });
    }

    for (std::size_t index = 0; index < targets.size(); ++index) {
        targets[index].candidate_index = index;
        targets[index].candidate_count = targets.size();
    }
    return targets;
}

std::optional<SmartTargetView> smart_target_at(
    const OverlayContext& context,
    double x,
    double y
) {
    auto targets = smart_targets_at(context, x, y);
    if (targets.empty()) return std::nullopt;

    const std::size_t index = context.smart_target_cycle % targets.size();
    return targets[index];
}

void reset_smart_target_cycle(OverlayContext* context, double x, double y, bool force = false) {
    if (context == nullptr) return;

    const auto targets = smart_targets_at(*context, x, y);
    const SemanticRegion* primary = targets.empty() ? nullptr : targets.front().region;

    constexpr double reset_distance = 8.0;
    const double delta_x = x - context->smart_cycle_anchor_x;
    const double delta_y = y - context->smart_cycle_anchor_y;
    const bool moved_far =
        delta_x * delta_x + delta_y * delta_y >= reset_distance * reset_distance;
    const bool primary_changed = primary != context->smart_cycle_anchor_primary;

    if (force || !context->smart_cycle_anchor_valid || moved_far || primary_changed) {
        context->smart_target_cycle = 0;
        context->smart_cycle_anchor_x = x;
        context->smart_cycle_anchor_y = y;
        context->smart_cycle_anchor_primary = primary;
        context->smart_cycle_anchor_valid = true;
    }
}

bool cycle_smart_target(OverlayContext* context, int direction) {
    if (context == nullptr || context->selecting || !context->pointer_inside || direction == 0) {
        return false;
    }

    auto targets = smart_targets_at(*context, context->pointer_x, context->pointer_y);
    if (targets.size() <= 1) return false;

    const std::size_t count = targets.size();
    const std::size_t current = context->smart_target_cycle % count;
    if (direction > 0) {
        context->smart_target_cycle = (current + 1) % count;
    } else {
        context->smart_target_cycle = (current + count - 1) % count;
    }
    context->smart_cycle_anchor_x = context->pointer_x;
    context->smart_cycle_anchor_y = context->pointer_y;
    context->smart_cycle_anchor_primary = targets.front().region;
    context->smart_cycle_anchor_valid = true;
    context->visual_smart_region = nullptr;
    refresh_hint(context);
    queue_canvas(context);
    return true;
}


SelectionRect pixel_rect_to_canvas(
    const OverlayContext& context,
    const PixelRect& rect
) {
    if (context.canvas == nullptr || context.frame == nullptr ||
        context.frame->width <= 0 || context.frame->height <= 0) {
        return {};
    }

    const int canvas_width = gtk_widget_get_width(context.canvas);
    const int canvas_height = gtk_widget_get_height(context.canvas);
    if (canvas_width <= 0 || canvas_height <= 0) return {};

    const double scale_x = static_cast<double>(canvas_width) /
        static_cast<double>(context.frame->width);
    const double scale_y = static_cast<double>(canvas_height) /
        static_cast<double>(context.frame->height);
    return SelectionRect{
        .x = rect.x * scale_x,
        .y = rect.y * scale_y,
        .width = rect.width * scale_x,
        .height = rect.height * scale_y,
    };
}

bool ocr_mode_active(const OverlayContext& context) {
    return context.ocr_region_selecting || context.ocr_processing ||
        context.ocr_revealing || context.ocr_ready;
}

void set_ratio_toolbar_visible(OverlayContext* context, bool visible) {
    if (context != nullptr && context->ratio_toolbar != nullptr) {
        gtk_widget_set_visible(context->ratio_toolbar, visible);
    }
}

void clear_ocr_mode(OverlayContext* context) {
    if (context == nullptr) return;
    if (context->ocr_thread.joinable()) {
        context->ocr_state.cancel_requested.store(true, std::memory_order_release);
        reap_finished_ocr_worker(context);
    }
    context->ocr_region_selecting = false;
    context->ocr_processing = false;
    context->ocr_revealing = false;
    context->ocr_ready = false;
    context->ocr_text_selecting = false;
    context->ocr_region_canvas = {};
    context->ocr_region_pixels = {};
    context->pending_ocr_words.clear();
    context->ocr_words.clear();
    context->ocr_anchor_word.reset();
    context->ocr_focus_word.reset();
    set_ratio_toolbar_visible(context, true);
    update_interaction_cursor(context);
    refresh_hint(context);
    queue_canvas(context);
}

std::optional<std::size_t> ocr_word_at(
    const OverlayContext& context,
    double x,
    double y,
    bool allow_nearest
) {
    if (!context.ocr_ready || context.ocr_words.empty()) return std::nullopt;

    std::optional<std::size_t> nearest;
    double nearest_distance_sq = 60.0 * 60.0;

    for (std::size_t index = 0; index < context.ocr_words.size(); ++index) {
        const SelectionRect rect = pixel_rect_to_canvas(context, context.ocr_words[index].rect);
        if (contains_point(rect, x, y)) return index;
        if (!allow_nearest) continue;

        const double dx = x < rect.x
            ? rect.x - x
            : x > rect.x + rect.width
                ? x - (rect.x + rect.width)
                : 0.0;
        const double dy = y < rect.y
            ? rect.y - y
            : y > rect.y + rect.height
                ? y - (rect.y + rect.height)
                : 0.0;
        const double distance_sq = dx * dx + dy * dy;
        if (distance_sq < nearest_distance_sq) {
            nearest_distance_sq = distance_sq;
            nearest = index;
        }
    }
    return nearest;
}

bool ocr_word_selected(const OverlayContext& context, std::size_t index) {
    if (!context.ocr_anchor_word || !context.ocr_focus_word) return false;
    const std::size_t first = std::min(*context.ocr_anchor_word, *context.ocr_focus_word);
    const std::size_t last = std::max(*context.ocr_anchor_word, *context.ocr_focus_word);
    return index >= first && index <= last;
}

std::string selected_ocr_text(const OverlayContext& context) {
    if (!context.ocr_anchor_word || !context.ocr_focus_word || context.ocr_words.empty()) {
        return {};
    }

    const std::size_t first = std::min(*context.ocr_anchor_word, *context.ocr_focus_word);
    const std::size_t last = std::min(
        std::max(*context.ocr_anchor_word, *context.ocr_focus_word),
        context.ocr_words.size() - 1
    );

    std::string output;
    const OcrWord* previous = nullptr;
    for (std::size_t index = first; index <= last; ++index) {
        const OcrWord& word = context.ocr_words[index];
        if (previous != nullptr) {
            if (word.block != previous->block || word.paragraph != previous->paragraph) {
                output += "\n\n";
            } else if (word.line != previous->line) {
                output += '\n';
            } else {
                output += ' ';
            }
        }
        output += word.text;
        previous = &word;
    }
    return output;
}

bool copy_selected_ocr_and_close(OverlayContext* context) {
    if (context == nullptr || context->window == nullptr || context->completed) return false;
    const std::string text = selected_ocr_text(*context);
    if (text.empty()) return false;

    std::string error;
    if (!ClipboardExporter::copy_text(text, error)) {
        std::cerr << "[Screenshot] " << error << '\n';
        show_transient_error(context, "Copy failed · " + error);
        return false;
    }

    context->completed = true;
    std::cout << "[Screenshot] Copied OCR text (" << text.size()
              << " bytes) to clipboard\n";
    gtk_window_destroy(context->window);
    return true;
}

SelectionRect current_ocr_region(const OverlayContext& context) {
    return normalize_selection(
        context.start_x,
        context.start_y,
        context.current_x,
        context.current_y
    );
}

void begin_ocr(OverlayContext* context, const SelectionRect& selection);

void rounded_rectangle(
    cairo_t* cr,
    double x,
    double y,
    double width,
    double height,
    double radius
) {
    const double right = x + width;
    const double bottom = y + height;

    cairo_new_sub_path(cr);
    cairo_arc(cr, right - radius, y + radius, radius, -kPi / 2.0, 0.0);
    cairo_arc(cr, right - radius, bottom - radius, radius, 0.0, kPi / 2.0);
    cairo_arc(cr, x + radius, bottom - radius, radius, kPi / 2.0, kPi);
    cairo_arc(cr, x + radius, y + radius, radius, kPi, 3.0 * kPi / 2.0);
    cairo_close_path(cr);
}

void draw_badge(
    cairo_t* cr,
    const std::string& text,
    double center_x,
    double center_y,
    int canvas_width,
    int canvas_height,
    const Rgba& accent = kAetherGold,
    double accent_strength = 0.95,
    double opacity = 1.0
) {
    cairo_save(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.5);

    cairo_text_extents_t extents{};
    cairo_text_extents(cr, text.c_str(), &extents);

    constexpr double pad_x = 10.0;
    constexpr double pad_y = 6.0;
    const double box_width = extents.width + pad_x * 2.0 + 10.0;
    const double box_height = extents.height + pad_y * 2.0;

    const double max_x = std::max(8.0, static_cast<double>(canvas_width) - box_width - 8.0);
    const double max_y = std::max(8.0, static_cast<double>(canvas_height) - box_height - 8.0);
    const double x = std::clamp(center_x - box_width / 2.0, 8.0, max_x);
    const double y = std::clamp(center_y - box_height / 2.0, 8.0, max_y);
    const double radius = 10.0;

    rounded_rectangle(cr, x, y, box_width, box_height, radius);
    set_source_rgba(cr, kBackdrop, opacity);
    cairo_fill_preserve(cr);

    cairo_set_line_width(cr, 1.0);
    set_source_rgba(cr, kBackdropBorder, opacity);
    cairo_stroke(cr);

    rounded_rectangle(cr, x + 3.0, y + 3.0, 4.0, box_height - 6.0, 2.0);
    set_source_rgba(cr, accent, accent_strength * opacity);
    cairo_fill(cr);

    set_source_rgba(cr, kTextWhite, 0.98 * opacity);
    cairo_move_to(
        cr,
        x + pad_x + 8.0 - extents.x_bearing,
        y + pad_y - extents.y_bearing
    );
    cairo_show_text(cr, text.c_str());
    cairo_restore(cr);
}

void draw_entry_sweep(cairo_t* cr, int width, int height, double progress) {
    const double t = ease_out_cubic(progress);
    const double band = std::max(72.0, static_cast<double>(height) * 0.21);
    const double center_y = -band + (static_cast<double>(height) + band * 2.0) * t;
    const double top = center_y - band;
    const double bottom = center_y + band;

    cairo_save(cr);
    cairo_pattern_t* glow = cairo_pattern_create_linear(0.0, top, 0.0, bottom);
    cairo_pattern_add_color_stop_rgba(glow, 0.00, 1.0, 1.0, 1.0, 0.00);
    cairo_pattern_add_color_stop_rgba(glow, 0.32, 1.0, 0.99, 0.96, 0.045);
    cairo_pattern_add_color_stop_rgba(glow, 0.50, 0.98, 0.93, 0.80, 0.16);
    cairo_pattern_add_color_stop_rgba(glow, 0.68, 1.0, 0.99, 0.96, 0.05);
    cairo_pattern_add_color_stop_rgba(glow, 1.00, 1.0, 1.0, 1.0, 0.00);
    cairo_rectangle(cr, 0.0, top, static_cast<double>(width), bottom - top);
    cairo_set_source(cr, glow);
    cairo_fill(cr);
    cairo_pattern_destroy(glow);

    cairo_pattern_t* core = cairo_pattern_create_linear(0.0, center_y - 8.0, 0.0, center_y + 8.0);
    cairo_pattern_add_color_stop_rgba(core, 0.00, 1.0, 1.0, 1.0, 0.00);
    cairo_pattern_add_color_stop_rgba(core, 0.50, 1.0, 0.98, 0.92, 0.36);
    cairo_pattern_add_color_stop_rgba(core, 1.00, 1.0, 1.0, 1.0, 0.00);
    cairo_rectangle(cr, 0.0, center_y - 8.0, static_cast<double>(width), 16.0);
    cairo_set_source(cr, core);
    cairo_fill(cr);
    cairo_pattern_destroy(core);

    const std::array<double, 4> anchors{0.18, 0.39, 0.64, 0.83};
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        const double x = anchors[i] * static_cast<double>(width) +
            std::sin((t + static_cast<double>(i) * 0.21) * kPi * 2.0) * 18.0;
        const double y = center_y + (static_cast<double>((i % 2) == 0 ? -1 : 1) * (12.0 + 7.0 * i));
        cairo_set_line_width(cr, 1.1);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, x - 12.0, y + 4.0);
        cairo_line_to(cr, x + 12.0, y - 4.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
        cairo_stroke(cr);

        cairo_arc(cr, x + 10.0, y - 2.0, 1.2 + 0.5 * (i % 2), 0.0, kPi * 2.0);
        cairo_set_source_rgba(cr, 1.0, 0.98, 0.92, 0.22);
        cairo_fill(cr);
    }
    cairo_restore(cr);
}

void draw_active_sweeps(cairo_t* cr, OverlayContext* context, int width, int height) {
    if (context == nullptr) return;
    const auto now = std::chrono::steady_clock::now();

    if (context->entry_sweep_active) {
        if (context->entry_sweep_pending_first_draw) {
            context->entry_sweep_started = now;
            context->entry_sweep_pending_first_draw = false;
            animate_until(context, now + kEntrySweepDuration);
        }
        draw_entry_sweep(
            cr,
            width,
            height,
            animation_progress(true, context->entry_sweep_started, kEntrySweepDuration)
        );
    }
}

void draw_hint_hud(
    cairo_t* cr,
    const OverlayContext& context,
    int width,
    int height,
    double opacity
) {
    std::string text;
    Rgba accent = kAetherGoldSoft;

    if (!context.transient_error.empty() &&
        std::chrono::steady_clock::now() < context.transient_error_until) {
        text = context.transient_error;
        accent = kAetherGold;
    } else if (context.ocr_processing) {
        text = "OCR  ·  Recognizing…";
        accent = kArcaneViolet;
    } else if (context.ocr_revealing) {
        text = "OCR  ·  Recognition complete  ·  revealing text…";
        accent = kArcaneViolet;
    } else if (context.ocr_ready) {
        text = "OCR  ·  LMB select  ·  Ctrl+A all  ·  Ctrl+C / Enter copy  ·  Esc back";
        accent = kArcaneViolet;
    } else if (context.ocr_region_selecting) {
        text = "OCR  ·  Release RMB to scan  ·  Esc cancel";
        accent = kArcaneViolet;
    } else if (context.selecting) {
        text = "Capture  ·  Release LMB to copy  ·  1 Free  ·  2 16:9  ·  3 1:1  ·  4 4:3";
    } else if (context.pointer_inside && context.visual_smart_region != nullptr) {
        text = "Smart  ·  LMB capture  ·  RMB OCR  ·  Tab / Wheel cycle";
    } else {
        text = "Capture  ·  LMB drag  ·  RMB drag OCR  ·  Tab / Wheel cycle  ·  Esc close";
    }

    cairo_save(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11.5);

    cairo_text_extents_t extents{};
    cairo_text_extents(cr, text.c_str(), &extents);

    constexpr double pad_x = 12.0;
    constexpr double pad_y = 5.0;
    constexpr double dot_space = 13.0;
    const double box_width = extents.width + pad_x * 2.0 + dot_space;
    const double box_height = extents.height + pad_y * 2.0;
    const double x = std::clamp(
        static_cast<double>(width) / 2.0 - box_width / 2.0,
        10.0,
        std::max(10.0, static_cast<double>(width) - box_width - 10.0)
    );
    const double y = std::max(10.0, static_cast<double>(height) - box_height - 82.0);

    rounded_rectangle(cr, x, y, box_width, box_height, box_height / 2.0);
    cairo_set_source_rgba(cr, 0.035, 0.035, 0.050, 0.70 * opacity);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.07 * opacity);
    cairo_stroke(cr);

    cairo_arc(cr, x + pad_x + 3.5, y + box_height / 2.0, 3.2, 0.0, kPi * 2.0);
    set_source_rgba(cr, accent, 0.96 * opacity);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.97, 0.97, 1.0, 0.86 * opacity);
    cairo_move_to(
        cr,
        x + pad_x + dot_space - extents.x_bearing,
        y + pad_y - extents.y_bearing
    );
    cairo_show_text(cr, text.c_str());
    cairo_restore(cr);
}

void draw_crosshair(cairo_t* cr, double x, double y, int width, int height) {
    cairo_save(cr);

    cairo_set_line_width(cr, 2.4);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.28);
    cairo_move_to(cr, x, 0.0);
    cairo_line_to(cr, x, static_cast<double>(height));
    cairo_move_to(cr, 0.0, y);
    cairo_line_to(cr, static_cast<double>(width), y);
    cairo_stroke(cr);

    cairo_set_line_width(cr, 0.9);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, kGuideAlpha);
    cairo_move_to(cr, x, 0.0);
    cairo_line_to(cr, x, static_cast<double>(height));
    cairo_move_to(cr, 0.0, y);
    cairo_line_to(cr, static_cast<double>(width), y);
    cairo_stroke(cr);

    // One compositor-independent precision reticle, rendered in the same
    // frame as the guide lines. Keep it roughly native-cursor sized so it is
    // easy to track without becoming a giant target over the screenshot.
    constexpr double arm = 14.0;
    constexpr double gap = 3.5;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    auto trace_reticle = [&]() {
        cairo_move_to(cr, x - arm, y);
        cairo_line_to(cr, x - gap, y);
        cairo_move_to(cr, x + gap, y);
        cairo_line_to(cr, x + arm, y);
        cairo_move_to(cr, x, y - arm);
        cairo_line_to(cr, x, y - gap);
        cairo_move_to(cr, x, y + gap);
        cairo_line_to(cr, x, y + arm);
    };

    // Broad dark silhouette keeps the reticle readable over bright windows.
    cairo_set_line_width(cr, 4.6);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.58);
    trace_reticle();
    cairo_stroke(cr);

    // Aether body.
    cairo_set_line_width(cr, 2.0);
    set_source_rgba(cr, kAetherGold, 0.98);
    trace_reticle();
    cairo_stroke(cr);

    // Fine neutral highlight makes the gold stay sharp on darker content
    // without turning the cursor into a glowing blob.
    cairo_set_line_width(cr, 0.75);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.74);
    trace_reticle();
    cairo_stroke(cr);

    cairo_restore(cr);
}

void draw_outside_dim(
    cairo_t* cr,
    const SelectionRect& selection,
    int width,
    int height,
    double alpha
) {
    const double right = selection.x + selection.width;
    const double bottom = selection.y + selection.height;

    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.02, 0.02, 0.03, alpha);

    cairo_rectangle(cr, 0.0, 0.0, static_cast<double>(width), selection.y);
    cairo_rectangle(
        cr,
        0.0,
        bottom,
        static_cast<double>(width),
        std::max(0.0, static_cast<double>(height) - bottom)
    );
    cairo_rectangle(cr, 0.0, selection.y, selection.x, selection.height);
    cairo_rectangle(
        cr,
        right,
        selection.y,
        std::max(0.0, static_cast<double>(width) - right),
        selection.height
    );
    cairo_fill(cr);

    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, selection.x + 0.5, selection.y + 0.5,
                    std::max(0.0, selection.width - 1.0),
                    std::max(0.0, selection.height - 1.0));
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.06);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void draw_selection_border(
    cairo_t* cr,
    const SelectionRect& selection,
    const Rgba& accent = kAetherGold,
    double fill_alpha = 0.06
) {
    const double x = selection.x + 0.5;
    const double y = selection.y + 0.5;
    const double width = std::max(0.0, selection.width - 1.0);
    const double height = std::max(0.0, selection.height - 1.0);
    if (width <= 0.0 || height <= 0.0) return;

    constexpr double radius = 10.0;
    constexpr double corner_len = 16.0;

    cairo_save(cr);
    rounded_rectangle(cr, x, y, width, height, radius);
    set_source_rgba(cr, accent, fill_alpha);
    cairo_fill_preserve(cr);

    cairo_set_line_width(cr, 2.8);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.40);
    cairo_stroke_preserve(cr);

    cairo_set_line_width(cr, 1.15);
    set_source_rgba(cr, alpha(accent, 0.96));
    cairo_stroke(cr);

    const double right = x + width;
    const double bottom = y + height;
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    // Soft dark under-stroke gives the Aether/Arcane corners contrast without
    // turning the entire selection into a glowing rectangle.
    cairo_set_line_width(cr, 4.0);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.26);
    cairo_move_to(cr, x + radius * 0.7, y);
    cairo_line_to(cr, std::min(right - radius, x + corner_len), y);
    cairo_move_to(cr, x, y + radius * 0.7);
    cairo_line_to(cr, x, std::min(bottom - radius, y + corner_len));
    cairo_move_to(cr, std::max(x + radius, right - corner_len), y);
    cairo_line_to(cr, right - radius * 0.7, y);
    cairo_move_to(cr, right, y + radius * 0.7);
    cairo_line_to(cr, right, std::min(bottom - radius, y + corner_len));
    cairo_move_to(cr, x + radius * 0.7, bottom);
    cairo_line_to(cr, std::min(right - radius, x + corner_len), bottom);
    cairo_move_to(cr, x, std::max(y + radius, bottom - corner_len));
    cairo_line_to(cr, x, bottom - radius * 0.7);
    cairo_move_to(cr, std::max(x + radius, right - corner_len), bottom);
    cairo_line_to(cr, right - radius * 0.7, bottom);
    cairo_move_to(cr, right, std::max(y + radius, bottom - corner_len));
    cairo_line_to(cr, right, bottom - radius * 0.7);
    cairo_stroke(cr);

    cairo_set_line_width(cr, 2.0);
    set_source_rgba(cr, accent, 0.98);
    cairo_move_to(cr, x + radius * 0.7, y);
    cairo_line_to(cr, std::min(right - radius, x + corner_len), y);
    cairo_move_to(cr, x, y + radius * 0.7);
    cairo_line_to(cr, x, std::min(bottom - radius, y + corner_len));
    cairo_move_to(cr, std::max(x + radius, right - corner_len), y);
    cairo_line_to(cr, right - radius * 0.7, y);
    cairo_move_to(cr, right, y + radius * 0.7);
    cairo_line_to(cr, right, std::min(bottom - radius, y + corner_len));
    cairo_move_to(cr, x + radius * 0.7, bottom);
    cairo_line_to(cr, std::min(right - radius, x + corner_len), bottom);
    cairo_move_to(cr, x, std::max(y + radius, bottom - corner_len));
    cairo_line_to(cr, x, bottom - radius * 0.7);
    cairo_move_to(cr, std::max(x + radius, right - corner_len), bottom);
    cairo_line_to(cr, right - radius * 0.7, bottom);
    cairo_move_to(cr, right, std::max(y + radius, bottom - corner_len));
    cairo_line_to(cr, right, bottom - radius * 0.7);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void draw_rule_of_thirds(cairo_t* cr, const SelectionRect& selection) {
    if (selection.width <= 0.0 || selection.height <= 0.0) return;

    const double x1 = selection.x + selection.width / 3.0;
    const double x2 = selection.x + selection.width * 2.0 / 3.0;
    const double y1 = selection.y + selection.height / 3.0;
    const double y2 = selection.y + selection.height * 2.0 / 3.0;

    cairo_save(cr);
    cairo_rectangle(cr, selection.x, selection.y, selection.width, selection.height);
    cairo_clip(cr);

    // Dark under-stroke keeps the guides readable on bright screenshots.
    cairo_set_line_width(cr, 2.0);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.34);
    cairo_move_to(cr, x1, selection.y);
    cairo_line_to(cr, x1, selection.y + selection.height);
    cairo_move_to(cr, x2, selection.y);
    cairo_line_to(cr, x2, selection.y + selection.height);
    cairo_move_to(cr, selection.x, y1);
    cairo_line_to(cr, selection.x + selection.width, y1);
    cairo_move_to(cr, selection.x, y2);
    cairo_line_to(cr, selection.x + selection.width, y2);
    cairo_stroke(cr);

    cairo_set_line_width(cr, 0.8);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.46);
    cairo_move_to(cr, x1, selection.y);
    cairo_line_to(cr, x1, selection.y + selection.height);
    cairo_move_to(cr, x2, selection.y);
    cairo_line_to(cr, x2, selection.y + selection.height);
    cairo_move_to(cr, selection.x, y1);
    cairo_line_to(cr, selection.x + selection.width, y1);
    cairo_move_to(cr, selection.x, y2);
    cairo_line_to(cr, selection.x + selection.width, y2);
    cairo_stroke(cr);
    cairo_restore(cr);
}

void draw_measurements(
    cairo_t* cr,
    const OverlayContext& context,
    const SelectionRect& selection,
    int width,
    int height
) {
    if (context.frame == nullptr) return;

    const PixelRect pixels = selection_to_pixels(
        selection,
        width,
        height,
        context.frame->width,
        context.frame->height
    );
    if (pixels.width <= 0 || pixels.height <= 0) return;

    const int left = pixels.x;
    const int top = pixels.y;
    const int right = std::max(0, context.frame->width - pixels.x - pixels.width);
    const int bottom = std::max(0, context.frame->height - pixels.y - pixels.height);

    std::string dimension_text =
        std::to_string(pixels.width) + " × " + std::to_string(pixels.height);
    if (selection_ratio_is_fixed(context.ratio)) {
        dimension_text += "  ·  ";
        dimension_text += selection_ratio_label(context.ratio);
    }

    draw_badge(
        cr,
        dimension_text,
        selection.x + selection.width / 2.0,
        std::max(20.0, selection.y - 20.0),
        width,
        height,
        kAetherGold
    );

    if (selection.x > 72.0) {
        draw_badge(
            cr,
            std::to_string(left) + " px",
            selection.x / 2.0,
            selection.y + selection.height / 2.0,
            width,
            height,
            kAetherGoldSoft,
            0.88
        );
    }
    if (static_cast<double>(width) - selection.x - selection.width > 72.0) {
        draw_badge(
            cr,
            std::to_string(right) + " px",
            selection.x + selection.width +
                (static_cast<double>(width) - selection.x - selection.width) / 2.0,
            selection.y + selection.height / 2.0,
            width,
            height,
            kAetherGoldSoft,
            0.88
        );
    }
    if (selection.y > 52.0) {
        draw_badge(
            cr,
            std::to_string(top) + " px",
            selection.x + selection.width / 2.0,
            selection.y / 2.0,
            width,
            height,
            kAetherGoldSoft,
            0.88
        );
    }
    if (static_cast<double>(height) - selection.y - selection.height > 52.0) {
        draw_badge(
            cr,
            std::to_string(bottom) + " px",
            selection.x + selection.width / 2.0,
            selection.y + selection.height +
                (static_cast<double>(height) - selection.y - selection.height) / 2.0,
            width,
            height,
            kAetherGoldSoft,
            0.88
        );
    }
}

SelectionRect inset_outline_rect(
    const SelectionRect& rect,
    int width,
    int height
) {
    constexpr double inset = 1.5;
    const double left = std::clamp(rect.x, inset, std::max(inset, static_cast<double>(width) - inset));
    const double top = std::clamp(rect.y, inset, std::max(inset, static_cast<double>(height) - inset));
    const double right = std::clamp(
        rect.x + rect.width,
        inset,
        std::max(inset, static_cast<double>(width) - inset)
    );
    const double bottom = std::clamp(
        rect.y + rect.height,
        inset,
        std::max(inset, static_cast<double>(height) - inset)
    );
    return SelectionRect{
        .x = left,
        .y = top,
        .width = std::max(0.0, right - left),
        .height = std::max(0.0, bottom - top),
    };
}

void draw_semantic_region_outlines(
    cairo_t* cr,
    OverlayContext& context,
    int width,
    int height
) {
    if (context.semantic_regions == nullptr || !context.semantic_regions->available) return;

    if (context.semantic_reveal_pending_first_draw) {
        context.semantic_reveal_started = std::chrono::steady_clock::now();
        context.semantic_reveal_pending_first_draw = false;
        animate_until(
            &context,
            context.semantic_reveal_started + kSemanticRevealTotalDuration
        );
    }

    cairo_save(cr);
    for (const auto& region : context.semantic_regions->regions) {
        const SelectionRect canvas_rect = semantic_rect_to_canvas(
            *context.semantic_regions,
            region.rect,
            width,
            height
        );
        const SelectionRect outline = inset_outline_rect(canvas_rect, width, height);
        if (outline.width <= 1.0 || outline.height <= 1.0) continue;

        const double reveal = semantic_reveal_progress(context, region.source);
        if (reveal <= 0.001) continue;

        const Rgba color = smart_target_color(region.source);
        const double base_alpha =
            region.source == SemanticRegionSource::Content
                ? kSemanticOutlineAlpha + 0.02
                : kSemanticOutlineAlpha - 0.06;

        if (context.semantic_reveal_active) {
            const double fill_alpha =
                region.source == SemanticRegionSource::Content
                    ? 0.018
                    : 0.024;
            draw_semantic_resolve_fx(
                cr,
                outline,
                color,
                reveal,
                base_alpha,
                fill_alpha
            );
        } else {
            rounded_rectangle(cr, outline.x, outline.y, outline.width, outline.height, 8.0);
            cairo_set_line_width(cr, 1.0);
            set_source_rgba(cr, color, base_alpha);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
}

void draw_smart_target(
    cairo_t* cr,
    const OverlayContext& context,
    const SmartTargetView& target,
    int width,
    int height,
    double emphasis
) {
    if (context.frame == nullptr || target.region == nullptr) return;

    const double reveal = semantic_reveal_progress(context, target.region->source);
    if (reveal <= 0.001) return;
    Rgba accent = smart_target_color(target.region->source);
    accent.a *= reveal;
    const double resolve_pulse = semantic_resolve_pulse(reveal);
    draw_outside_dim(
        cr,
        target.rect,
        width,
        height,
        kSmartTargetDimAlpha * (0.30 + 0.70 * reveal)
    );
    const SelectionRect outline = inset_outline_rect(target.rect, width, height);
    cairo_save(cr);
    rounded_rectangle(
        cr,
        outline.x - 2.0,
        outline.y - 2.0,
        outline.width + 4.0,
        outline.height + 4.0,
        11.0
    );
    cairo_set_line_width(cr, 1.0 + 1.1 * emphasis + 0.2 * resolve_pulse);
    set_source_rgba(cr, accent, 0.10 + 0.12 * emphasis + 0.10 * resolve_pulse);
    cairo_stroke(cr);
    cairo_restore(cr);
    draw_selection_border(
        cr,
        outline,
        accent,
        0.045 + 0.028 * emphasis + 0.022 * resolve_pulse
    );

    const PixelRect pixels = selection_to_pixels(
        target.rect,
        width,
        height,
        context.frame->width,
        context.frame->height
    );
    if (pixels.width <= 0 || pixels.height <= 0) return;

    std::string label = semantic_region_source_label(target.region->source);
    if (target.candidate_count > 1) {
        label += " ";
        label += std::to_string(target.candidate_index + 1);
        label += "/";
        label += std::to_string(target.candidate_count);
    }
    label += "  ·  ";
    label += std::to_string(pixels.width);
    label += " × ";
    label += std::to_string(pixels.height);

    draw_badge(
        cr,
        label,
        target.rect.x + target.rect.width / 2.0,
        std::max(20.0, target.rect.y - 20.0),
        width,
        height,
        accent,
        0.95 + 0.05 * resolve_pulse,
        reveal * (0.84 + 0.16 * emphasis)
    );
}

void draw_ocr_reveal_scan(
    cairo_t* cr,
    OverlayContext& context,
    const SelectionRect& region
) {
    if (!context.ocr_revealing || region.width <= 1.0 || region.height <= 1.0) return;

    const auto now = std::chrono::steady_clock::now();
    if (context.ocr_sweep_pending_first_draw) {
        context.ocr_sweep_started = now;
        context.ocr_sweep_pending_first_draw = false;
        animate_until(&context, now + kOcrSweepDuration);
    }

    const double progress = animation_progress(
        true,
        context.ocr_sweep_started,
        kOcrSweepDuration
    );
    const double t = ease_in_out(progress);
    const double left = region.x;
    const double right = region.x + region.width;
    const double bottom = region.y + region.height;
    const double scan_y = bottom - region.height * t;

    cairo_save(cr);
    rounded_rectangle(cr, region.x, region.y, region.width, region.height, 10.0);
    cairo_clip(cr);

    // The portion already traversed by the scanner gets a faint acquired tint.
    cairo_rectangle(cr, left, scan_y, region.width, std::max(0.0, bottom - scan_y));
    cairo_set_source_rgba(cr, 0.50, 0.38, 0.92, 0.085);
    cairo_fill(cr);

    // Broad halo around the scanner line. Deliberately visible, but still transparent.
    constexpr int halo_steps = 7;
    for (int step = halo_steps; step >= 1; --step) {
        const double half_height = 3.0 + static_cast<double>(step) * 3.6;
        const double a = 0.018 + static_cast<double>(halo_steps - step) * 0.007;
        cairo_rectangle(cr, left, scan_y - half_height, region.width, half_height * 2.0);
        cairo_set_source_rgba(cr, 0.68, 0.57, 1.0, a);
        cairo_fill(cr);
    }

    // Dark under-line keeps the scan readable over bright white content.
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 6.0);
    cairo_set_source_rgba(cr, 0.02, 0.02, 0.05, 0.48);
    cairo_move_to(cr, left + 5.0, scan_y + 1.0);
    cairo_line_to(cr, right - 5.0, scan_y + 1.0);
    cairo_stroke(cr);

    // Main recognition beam: violet-white with a thin Aether accent immediately behind it.
    cairo_set_line_width(cr, 2.4);
    cairo_set_source_rgba(cr, 0.90, 0.86, 1.0, 0.96);
    cairo_move_to(cr, left + 5.0, scan_y);
    cairo_line_to(cr, right - 5.0, scan_y);
    cairo_stroke(cr);

    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 0.97, 0.83, 0.38, 0.76);
    cairo_move_to(cr, left + 14.0, scan_y + 3.0);
    cairo_line_to(cr, right - 14.0, scan_y + 3.0);
    cairo_stroke(cr);

    // Recognition traces trail the beam in short deterministic segments.
    const std::array<double, 5> trail_offsets{12.0, 23.0, 35.0, 49.0, 64.0};
    for (std::size_t row = 0; row < trail_offsets.size(); ++row) {
        const double y = scan_y + trail_offsets[row];
        if (y > bottom) continue;

        double x = left + 12.0 + static_cast<double>(row) * 7.0;
        const double segment = 14.0 + static_cast<double>(row % 3) * 9.0;
        while (x < right - 10.0) {
            const double w = std::min(segment, right - x - 8.0);
            if (w > 2.0) {
                rounded_rectangle(cr, x, y, w, 1.5, 0.75);
            }
            x += segment + 11.0 + static_cast<double>((row + 1) % 3) * 6.0;
        }
        cairo_set_source_rgba(
            cr,
            0.73,
            0.66,
            1.0,
            row == 0 ? 0.42 : std::max(0.10, 0.28 - static_cast<double>(row) * 0.04)
        );
        cairo_fill(cr);
    }

    // Small vertical acquisition ticks immediately behind the beam.
    const double ticks_y = scan_y + 9.0;
    if (ticks_y <= bottom) {
        for (double x = left + 22.0; x < right - 18.0; x += 46.0) {
            cairo_rectangle(cr, x, ticks_y, 1.2, 7.0);
        }
        cairo_set_source_rgba(cr, 0.94, 0.91, 1.0, 0.34);
        cairo_fill(cr);
    }

    cairo_restore(cr);
}

void draw_ocr_region_state(
    cairo_t* cr,
    OverlayContext& context,
    int width,
    int height
) {
    SelectionRect region = context.ocr_region_canvas;
    if (context.ocr_region_selecting) {
        region = current_ocr_region(context);
    }
    if (region.width <= 0.0 || region.height <= 0.0) return;

    draw_outside_dim(cr, region, width, height, kDimAlpha);
    draw_selection_border(cr, region, kArcaneViolet, 0.08);

    if (context.ocr_processing) {
        draw_badge(
            cr,
            "OCR · Recognizing…",
            region.x + region.width / 2.0,
            std::max(20.0, region.y - 20.0),
            width,
            height,
            kArcaneViolet
        );
        return;
    }

    if (context.ocr_region_selecting && context.frame != nullptr) {
        const PixelRect pixels = selection_to_pixels(
            region,
            width,
            height,
            context.frame->width,
            context.frame->height
        );
        draw_badge(
            cr,
            "OCR · " + std::to_string(pixels.width) + " × " + std::to_string(pixels.height),
            region.x + region.width / 2.0,
            std::max(20.0, region.y - 20.0),
            width,
            height,
            kArcaneViolet
        );
        return;
    }

    if (context.ocr_revealing) {
        draw_ocr_reveal_scan(cr, context, region);
        draw_badge(
            cr,
            "OCR · Scanning recognized text…",
            region.x + region.width / 2.0,
            std::max(20.0, region.y - 20.0),
            width,
            height,
            kArcaneViolet
        );
        return;
    }

    if (!context.ocr_ready) return;

    for (std::size_t index = 0; index < context.ocr_words.size(); ++index) {
        const SelectionRect word_rect = pixel_rect_to_canvas(context, context.ocr_words[index].rect);
        if (word_rect.width <= 0.0 || word_rect.height <= 0.0) continue;

        cairo_save(cr);
        rounded_rectangle(cr, word_rect.x, word_rect.y, word_rect.width, word_rect.height, 4.0);
        if (ocr_word_selected(context, index)) {
            set_source_rgba(cr, kArcaneViolet, 0.26);
            cairo_fill_preserve(cr);
            cairo_set_line_width(cr, 1.2);
            set_source_rgba(cr, kAetherGold, 0.96);
        } else {
            set_source_rgba(cr, kArcaneVioletSoft, 0.12);
            cairo_fill_preserve(cr);
            cairo_set_line_width(cr, 0.85);
            set_source_rgba(cr, kArcaneViolet, 0.52);
        }
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    std::string badge = "OCR · " + std::to_string(context.ocr_words.size()) + " words";
    if (context.ocr_anchor_word && context.ocr_focus_word) {
        const std::size_t first = std::min(*context.ocr_anchor_word, *context.ocr_focus_word);
        const std::size_t last = std::max(*context.ocr_anchor_word, *context.ocr_focus_word);
        badge += " · " + std::to_string(last - first + 1) + " selected";
    }
    badge += " · Ctrl+C / Enter";
    draw_badge(
        cr,
        badge,
        region.x + region.width / 2.0,
        std::max(20.0, region.y - 20.0),
        width,
        height,
        kArcaneViolet
    );
}

bool copy_selection_and_close(
    OverlayContext* context,
    const SelectionRect& selection,
    const char* kind
) {
    if (context == nullptr || context->canvas == nullptr ||
        context->frame == nullptr || context->window == nullptr ||
        context->completed) {
        return false;
    }

    const int width = gtk_widget_get_width(context->canvas);
    const int height = gtk_widget_get_height(context->canvas);
    const PixelRect pixels = selection_to_pixels(
        selection,
        width,
        height,
        context->frame->width,
        context->frame->height
    );
    if (pixels.width <= 1 || pixels.height <= 1) return false;

    std::string error;
    if (!ClipboardExporter::copy_png(*context->frame, pixels, error)) {
        std::cerr << "[Screenshot] " << error << '\n';
        show_transient_error(context, "Copy failed · " + error);
        return false;
    }

    context->completed = true;
    std::cout << "[Screenshot] Copied "
              << pixels.width << 'x' << pixels.height << ' '
              << kind << " to clipboard\n";
    gtk_window_destroy(context->window);
    return true;
}


gboolean poll_ocr_result(gpointer user_data) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr) return G_SOURCE_REMOVE;
    if (context->shutting_down) {
        context->ocr_poll_source = 0;
        return G_SOURCE_REMOVE;
    }
    if (!context->ocr_state.ready.load(std::memory_order_acquire)) {
        return G_SOURCE_CONTINUE;
    }

    std::optional<OcrResult> loaded;
    {
        std::lock_guard lock(context->ocr_state.mutex);
        if (context->ocr_state.result.has_value()) {
            loaded = std::move(context->ocr_state.result);
            context->ocr_state.result.reset();
        }
    }
    context->ocr_poll_source = 0;
    if (context->ocr_thread.joinable()) {
        context->ocr_thread.join();
    }
    context->ocr_state.ready.store(false, std::memory_order_release);

    if (!loaded.has_value() || context->window == nullptr) {
        return G_SOURCE_REMOVE;
    }

    // Esc can cancel OCR while the worker is running. In that case the result
    // is intentionally discarded and the normal capture UI remains active.
    if (!context->ocr_processing ||
        context->ocr_state.cancel_requested.load(std::memory_order_acquire)) {
        context->ocr_state.cancel_requested.store(false, std::memory_order_release);
        return G_SOURCE_REMOVE;
    }

    context->ocr_processing = false;
    context->ocr_state.cancel_requested.store(false, std::memory_order_release);

    if (!loaded->ok) {
        std::cerr << "[Screenshot] OCR failed: " << loaded->error << '\n';
        const std::string message = "OCR failed · " + loaded->error;
        clear_ocr_mode(context);
        show_transient_error(context, message);
        return G_SOURCE_REMOVE;
    }

    context->pending_ocr_words = std::move(loaded->words);
    context->ocr_words.clear();
    context->ocr_revealing = true;
    context->ocr_ready = false;
    context->ocr_anchor_word.reset();
    context->ocr_focus_word.reset();
    start_ocr_sweep(context);
    update_interaction_cursor(context);
    refresh_hint(context);
    std::cout << "[Screenshot] OCR recognized " << context->pending_ocr_words.size()
              << " word(s)\n";
    queue_canvas(context);
    return G_SOURCE_REMOVE;
}

void begin_ocr(OverlayContext* context, const SelectionRect& selection) {
    if (context == nullptr || context->shutting_down || context->canvas == nullptr ||
        context->frame == nullptr || context->ocr_processing || context->ocr_revealing ||
        context->completed) {
        return;
    }

    if (context->ocr_thread.joinable() && !reap_finished_ocr_worker(context)) {
        show_transient_error(context, "OCR cancellation finishing · try again");
        return;
    }

    std::string ocr_error;
    if (!OcrEngine::available(ocr_error)) {
        std::cerr << "[Screenshot] OCR unavailable: " << ocr_error << '\n';
        show_transient_error(context, "OCR unavailable · " + ocr_error);
        return;
    }

    const int width = gtk_widget_get_width(context->canvas);
    const int height = gtk_widget_get_height(context->canvas);
    const PixelRect pixels = selection_to_pixels(
        selection,
        width,
        height,
        context->frame->width,
        context->frame->height
    );
    if (pixels.width <= 2 || pixels.height <= 2) return;

    context->selecting = false;
    context->ocr_region_selecting = false;
    context->ocr_text_selecting = false;
    context->ocr_revealing = false;
    context->ocr_ready = false;
    context->ocr_processing = true;
    context->ocr_region_canvas = selection;
    context->ocr_region_pixels = pixels;
    context->pending_ocr_words.clear();
    context->ocr_words.clear();
    context->ocr_anchor_word.reset();
    context->ocr_focus_word.reset();
    set_ratio_toolbar_visible(context, false);
    update_interaction_cursor(context);
    refresh_hint(context);
    queue_canvas(context);

    context->ocr_state.ready.store(false, std::memory_order_release);
    context->ocr_state.cancel_requested.store(false, std::memory_order_release);
    {
        std::lock_guard lock(context->ocr_state.mutex);
        context->ocr_state.result.reset();
    }

    const FrozenFrame* frame = context->frame;
    OcrState* state = &context->ocr_state;
    try {
        context->ocr_thread = std::thread([state, frame, pixels]() {
            OcrResult result;
            try {
                result = OcrEngine::recognize(
                    *frame,
                    pixels,
                    &state->cancel_requested
                );
            } catch (const std::exception& exception) {
                result.error = std::string{"OCR worker exception: "} + exception.what();
            } catch (...) {
                result.error = "OCR worker failed with an unknown exception";
            }
            {
                std::lock_guard lock(state->mutex);
                state->result = std::move(result);
            }
            state->ready.store(true, std::memory_order_release);
        });
    } catch (const std::exception& exception) {
        context->ocr_processing = false;
        context->ocr_state.cancel_requested.store(false, std::memory_order_release);
        set_ratio_toolbar_visible(context, true);
        update_interaction_cursor(context);
        show_transient_error(
            context,
            std::string{"Unable to start OCR worker · "} + exception.what()
        );
        return;
    }

    context->ocr_poll_source = g_timeout_add(12, poll_ocr_result, context);
    if (context->ocr_poll_source == 0) {
        context->ocr_state.cancel_requested.store(true, std::memory_order_release);
        context->ocr_thread.join();
        context->ocr_processing = false;
        context->ocr_state.ready.store(false, std::memory_order_release);
        {
            std::lock_guard lock(context->ocr_state.mutex);
            context->ocr_state.result.reset();
        }
        context->ocr_state.cancel_requested.store(false, std::memory_order_release);
        set_ratio_toolbar_visible(context, true);
        update_interaction_cursor(context);
        show_transient_error(context, "Unable to schedule OCR completion handler");
    }
}

void draw_overlay(
    GtkDrawingArea*,
    cairo_t* cr,
    int width,
    int height,
    gpointer user_data
) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr) return;

    if (ocr_mode_active(*context)) {
        draw_ocr_region_state(cr, *context, width, height);
    } else if (context->selecting) {
        const SelectionRect selection = current_selection(*context);
        if (selection.width > 0.0 && selection.height > 0.0) {
            draw_outside_dim(cr, selection, width, height, kDimAlpha);
            if (selection_ratio_is_fixed(context->ratio)) {
                draw_rule_of_thirds(cr, selection);
            }
            draw_selection_border(cr, selection, kAetherGold, 0.06);
            draw_measurements(cr, *context, selection, width, height);
        }
    } else {
        draw_semantic_region_outlines(cr, *context, width, height);
        if (context->pointer_inside) {
            if (const auto target = smart_target_at(
                *context,
                context->pointer_x,
                context->pointer_y
            )) {
                if (context->visual_smart_region != target->region ||
                    context->visual_smart_candidate != target->candidate_index) {
                    context->visual_smart_region = target->region;
                    context->visual_smart_candidate = target->candidate_index;
                    context->smart_emphasis_started = std::chrono::steady_clock::now();
                    animate_until(context, context->smart_emphasis_started + kSmartEmphasisDuration);
                }

                const auto elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - context->smart_emphasis_started
                ).count();
                const double linear = std::clamp(
                    elapsed / static_cast<double>(kSmartEmphasisDuration.count()),
                    0.0,
                    1.0
                );
                const double emphasis = 1.0 - std::pow(1.0 - linear, 3.0);
                draw_smart_target(cr, *context, *target, width, height, emphasis);
            } else {
                context->visual_smart_region = nullptr;
            }
        }
    }

    if ((context->pointer_inside || context->selecting || context->ocr_region_selecting) &&
        !context->ocr_ready && !context->ocr_processing && !context->ocr_revealing) {
        draw_crosshair(
            cr,
            context->pointer_x,
            context->pointer_y,
            width,
            height
        );
    }

    draw_active_sweeps(cr, context, width, height);
    draw_hint_hud(cr, *context, width, height, hint_opacity(*context));
}

gboolean on_key_pressed(
    GtkEventControllerKey*,
    guint keyval,
    guint,
    GdkModifierType state,
    gpointer user_data
) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down || context->window == nullptr) {
        return GDK_EVENT_PROPAGATE;
    }

    if (keyval == GDK_KEY_Escape) {
        if (ocr_mode_active(*context)) {
            clear_ocr_mode(context);
        } else {
            gtk_window_destroy(context->window);
        }
        return GDK_EVENT_STOP;
    }

    if (context->ocr_ready) {
        const bool control = (state & GDK_CONTROL_MASK) != 0;
        if (control && (keyval == GDK_KEY_a || keyval == GDK_KEY_A)) {
            if (!context->ocr_words.empty()) {
                context->ocr_anchor_word = 0;
                context->ocr_focus_word = context->ocr_words.size() - 1;
                queue_canvas(context);
            }
            return GDK_EVENT_STOP;
        }
        if ((control && (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) ||
            keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            if (copy_selected_ocr_and_close(context)) {
                return GDK_EVENT_STOP;
            }
            return GDK_EVENT_STOP;
        }
        return GDK_EVENT_PROPAGATE;
    }

    if (context->ocr_processing || context->ocr_revealing || context->ocr_region_selecting) {
        return GDK_EVENT_PROPAGATE;
    }

    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
        const bool reverse = keyval == GDK_KEY_ISO_Left_Tab ||
            (state & GDK_SHIFT_MASK) != 0;
        if (cycle_smart_target(context, reverse ? -1 : 1)) {
            return GDK_EVENT_STOP;
        }
    }

    int ratio_index = -1;
    if (keyval == GDK_KEY_1 || keyval == GDK_KEY_KP_1) ratio_index = 0;
    if (keyval == GDK_KEY_2 || keyval == GDK_KEY_KP_2) ratio_index = 1;
    if (keyval == GDK_KEY_3 || keyval == GDK_KEY_KP_3) ratio_index = 2;
    if (keyval == GDK_KEY_4 || keyval == GDK_KEY_KP_4) ratio_index = 3;

    if (ratio_index >= 0) {
        context->ratio = static_cast<SelectionRatio>(ratio_index);
        GtkToggleButton* button = context->ratio_buttons[static_cast<std::size_t>(ratio_index)];
        if (button != nullptr && !gtk_toggle_button_get_active(button)) {
            gtk_toggle_button_set_active(button, true);
        }
        queue_canvas(context);
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

void on_motion_enter(
    GtkEventControllerMotion*,
    double x,
    double y,
    gpointer user_data
) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down || context->canvas == nullptr) return;

    context->pointer_inside = true;
    context->pointer_x = x;
    context->pointer_y = y;
    reset_smart_target_cycle(context, x, y, true);
    queue_canvas(context);
}

void on_motion(
    GtkEventControllerMotion*,
    double x,
    double y,
    gpointer user_data
) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down || context->canvas == nullptr) return;

    const int width = gtk_widget_get_width(context->canvas);
    const int height = gtk_widget_get_height(context->canvas);
    context->pointer_inside = true;
    const double next_x = clamp_coordinate(x, width);
    const double next_y = clamp_coordinate(y, height);
    if (!context->selecting && !ocr_mode_active(*context)) {
        reset_smart_target_cycle(context, next_x, next_y);
    }
    context->pointer_x = next_x;
    context->pointer_y = next_y;

    if (context->selecting || context->ocr_region_selecting) {
        context->current_x = context->pointer_x;
        context->current_y = context->pointer_y;
    }
    if (context->ocr_text_selecting) {
        if (const auto word = ocr_word_at(*context, context->pointer_x, context->pointer_y, true)) {
            context->ocr_focus_word = *word;
        }
    }
    queue_canvas(context);
}

void on_motion_leave(GtkEventControllerMotion*, gpointer user_data) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down) return;

    if (!context->selecting && !context->ocr_region_selecting && !context->ocr_text_selecting) {
        context->pointer_inside = false;
        context->visual_smart_region = nullptr;
    }
    queue_canvas(context);
}

gboolean on_scroll(
    GtkEventControllerScroll*,
    double,
    double delta_y,
    gpointer user_data
) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down || ocr_mode_active(*context) ||
        std::abs(delta_y) < 0.01) {
        return GDK_EVENT_PROPAGATE;
    }

    if (cycle_smart_target(context, delta_y > 0.0 ? 1 : -1)) {
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

void on_drag_begin(
    GtkGestureDrag*,
    double start_x,
    double start_y,
    gpointer user_data
) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down || context->canvas == nullptr) return;

    const int width = gtk_widget_get_width(context->canvas);
    const int height = gtk_widget_get_height(context->canvas);

    if (context->ocr_ready) {
        context->ocr_text_selecting = true;
        context->pointer_inside = true;
        context->pointer_x = clamp_coordinate(start_x, width);
        context->pointer_y = clamp_coordinate(start_y, height);
        context->ocr_anchor_word = ocr_word_at(
            *context,
            context->pointer_x,
            context->pointer_y,
            false
        );
        context->ocr_focus_word = context->ocr_anchor_word;
        queue_canvas(context);
        return;
    }
    if (context->ocr_processing || context->ocr_revealing || context->ocr_region_selecting) return;

    context->selecting = true;
    context->pointer_inside = true;
    context->start_x = clamp_coordinate(start_x, width);
    context->start_y = clamp_coordinate(start_y, height);
    context->current_x = context->start_x;
    context->current_y = context->start_y;
    context->pointer_x = context->start_x;
    context->pointer_y = context->start_y;
    refresh_hint(context);
    queue_canvas(context);
}

void on_drag_update(
    GtkGestureDrag* gesture,
    double offset_x,
    double offset_y,
    gpointer user_data
) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down || context->canvas == nullptr) return;

    const int width = gtk_widget_get_width(context->canvas);
    const int height = gtk_widget_get_height(context->canvas);

    if (context->ocr_text_selecting) {
        double start_x = 0.0;
        double start_y = 0.0;
        if (!gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y)) return;
        context->pointer_x = clamp_coordinate(start_x + offset_x, width);
        context->pointer_y = clamp_coordinate(start_y + offset_y, height);
        if (const auto word = ocr_word_at(*context, context->pointer_x, context->pointer_y, true)) {
            context->ocr_focus_word = *word;
        }
        queue_canvas(context);
        return;
    }

    if (!context->selecting) return;
    context->current_x = clamp_coordinate(context->start_x + offset_x, width);
    context->current_y = clamp_coordinate(context->start_y + offset_y, height);
    context->pointer_x = context->current_x;
    context->pointer_y = context->current_y;
    queue_canvas(context);
}

void on_drag_end(
    GtkGestureDrag* gesture,
    double offset_x,
    double offset_y,
    gpointer user_data
) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down || context->canvas == nullptr ||
        context->frame == nullptr || context->window == nullptr) {
        return;
    }

    const int width = gtk_widget_get_width(context->canvas);
    const int height = gtk_widget_get_height(context->canvas);

    if (context->ocr_text_selecting) {
        double start_x = 0.0;
        double start_y = 0.0;
        if (gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y)) {
            context->pointer_x = clamp_coordinate(start_x + offset_x, width);
            context->pointer_y = clamp_coordinate(start_y + offset_y, height);
            if (const auto word = ocr_word_at(
                    *context,
                    context->pointer_x,
                    context->pointer_y,
                    true
                )) {
                context->ocr_focus_word = *word;
            }
        }
        context->ocr_text_selecting = false;
        queue_canvas(context);
        return;
    }

    if (!context->selecting) return;
    context->current_x = clamp_coordinate(context->start_x + offset_x, width);
    context->current_y = clamp_coordinate(context->start_y + offset_y, height);
    context->pointer_x = context->current_x;
    context->pointer_y = context->current_y;

    const SelectionRect selection = current_selection(*context);
    if (
        selection.width < kMinimumSelectionLogical ||
        selection.height < kMinimumSelectionLogical
    ) {
        context->selecting = false;
        if (const auto target = smart_target_at(
                *context,
                context->pointer_x,
                context->pointer_y
            )) {
            if (copy_selection_and_close(context, target->rect, "smart target")) {
                return;
            }
        }
        queue_canvas(context);
        return;
    }

    if (!copy_selection_and_close(context, selection, "region")) {
        context->selecting = false;
        queue_canvas(context);
    }
}


void on_ocr_drag_begin(
    GtkGestureDrag*,
    double start_x,
    double start_y,
    gpointer user_data
) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down || context->canvas == nullptr ||
        context->ocr_processing || context->ocr_revealing || context->ocr_ready || context->completed) {
        return;
    }

    const int width = gtk_widget_get_width(context->canvas);
    const int height = gtk_widget_get_height(context->canvas);
    context->ocr_region_selecting = true;
    context->selecting = false;
    context->pointer_inside = true;
    context->start_x = clamp_coordinate(start_x, width);
    context->start_y = clamp_coordinate(start_y, height);
    context->current_x = context->start_x;
    context->current_y = context->start_y;
    context->pointer_x = context->start_x;
    context->pointer_y = context->start_y;
    set_ratio_toolbar_visible(context, false);
    refresh_hint(context);
    queue_canvas(context);
}

void on_ocr_drag_update(
    GtkGestureDrag*,
    double offset_x,
    double offset_y,
    gpointer user_data
) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down || context->canvas == nullptr ||
        !context->ocr_region_selecting) {
        return;
    }

    const int width = gtk_widget_get_width(context->canvas);
    const int height = gtk_widget_get_height(context->canvas);
    context->current_x = clamp_coordinate(context->start_x + offset_x, width);
    context->current_y = clamp_coordinate(context->start_y + offset_y, height);
    context->pointer_x = context->current_x;
    context->pointer_y = context->current_y;
    queue_canvas(context);
}

void on_ocr_drag_end(
    GtkGestureDrag*,
    double offset_x,
    double offset_y,
    gpointer user_data
) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down || context->canvas == nullptr ||
        context->frame == nullptr || !context->ocr_region_selecting) {
        return;
    }

    const int width = gtk_widget_get_width(context->canvas);
    const int height = gtk_widget_get_height(context->canvas);
    context->current_x = clamp_coordinate(context->start_x + offset_x, width);
    context->current_y = clamp_coordinate(context->start_y + offset_y, height);
    context->pointer_x = context->current_x;
    context->pointer_y = context->current_y;

    SelectionRect selection = current_ocr_region(*context);
    if (selection.width < kMinimumSelectionLogical ||
        selection.height < kMinimumSelectionLogical) {
        context->ocr_region_selecting = false;
        if (const auto target = smart_target_at(
                *context,
                context->pointer_x,
                context->pointer_y
            )) {
            begin_ocr(context, target->rect);
            return;
        }
        set_ratio_toolbar_visible(context, true);
        queue_canvas(context);
        return;
    }

    begin_ocr(context, selection);
}

void on_ratio_toggled(GtkToggleButton* button, gpointer user_data) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || !gtk_toggle_button_get_active(button)) return;

    for (std::size_t index = 0; index < context->ratio_buttons.size(); ++index) {
        if (context->ratio_buttons[index] == button) {
            context->ratio = static_cast<SelectionRatio>(index);
            refresh_hint(context);
            queue_canvas(context);
            return;
        }
    }
}

GtkWidget* make_ratio_toolbar(OverlayContext* context) {
    if (context == nullptr) return nullptr;

    GtkWidget* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(toolbar, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(toolbar, GTK_ALIGN_END);
    gtk_widget_set_margin_bottom(toolbar, 24);
    gtk_widget_add_css_class(toolbar, "rh-shot-toolbar");

    constexpr std::array<const char*, 4> labels{
        "Free",
        "16:9",
        "1:1",
        "4:3",
    };

    GtkToggleButton* group = nullptr;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        GtkWidget* widget = gtk_toggle_button_new_with_label(labels[index]);
        auto* button = GTK_TOGGLE_BUTTON(widget);
        context->ratio_buttons[index] = button;

        if (group == nullptr) {
            group = button;
        } else {
            gtk_toggle_button_set_group(button, group);
        }

        gtk_widget_set_focusable(widget, false);
        gtk_widget_add_css_class(widget, "rh-shot-ratio-button");
        gtk_widget_set_tooltip_text(
            widget,
            index == 0 ? "Free selection (1)" :
            index == 1 ? "16:9 selection (2)" :
            index == 2 ? "1:1 selection (3)" :
                         "4:3 selection (4)"
        );
        g_signal_connect(button, "toggled", G_CALLBACK(on_ratio_toggled), context);
        gtk_box_append(GTK_BOX(toolbar), widget);
    }

    gtk_toggle_button_set_active(context->ratio_buttons[0], true);
    return toolbar;
}

bool screenshot_timing_enabled() {
    return std::getenv("REALMHEART_SCREENSHOT_TIMING") != nullptr;
}

void print_smart_target_summary(
    const SemanticRegionSnapshot& snapshot,
    const std::string& connector
) {
    if (!snapshot.available) return;

    std::size_t contents = 0;
    std::size_t windows = 0;
    std::size_t layers = 0;
    for (const auto& region : snapshot.regions) {
        switch (region.source) {
            case SemanticRegionSource::Content:
                ++contents;
                break;
            case SemanticRegionSource::Layer:
                ++layers;
                break;
            case SemanticRegionSource::Window:
            default:
                ++windows;
                break;
        }
    }

    std::cout << "[Screenshot] Smart targets: "
              << contents << " content, "
              << windows << " window(s), "
              << layers << " layer(s) on "
              << connector << '\n';

    if (std::getenv("REALMHEART_SCREENSHOT_DEBUG") != nullptr) {
        for (const auto& region : snapshot.regions) {
            std::cout << "[Screenshot]   "
                      << semantic_region_source_label(region.source)
                      << " x=" << region.rect.x
                      << " y=" << region.rect.y
                      << " w=" << region.rect.width
                      << " h=" << region.rect.height
                      << "  " << region.label << '\n';
        }
    }
}

gboolean poll_smart_region_detection(gpointer user_data) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr) return G_SOURCE_REMOVE;
    if (context->shutting_down) {
        context->detection_poll_source = 0;
        return G_SOURCE_REMOVE;
    }
    if (!context->detection_state.ready.load(std::memory_order_acquire)) {
        return G_SOURCE_CONTINUE;
    }

    std::optional<SmartRegionLoadResult> loaded;
    {
        std::lock_guard lock(context->detection_state.mutex);
        if (context->detection_state.result.has_value()) {
            loaded = std::move(context->detection_state.result);
            context->detection_state.result.reset();
        }
    }

    context->detection_poll_source = 0;
    if (context->detection_thread.joinable()) {
        context->detection_thread.join();
    }
    if (!loaded.has_value() || context->window == nullptr) return G_SOURCE_REMOVE;

    context->smart_cycle_anchor_primary = nullptr;
    context->smart_cycle_anchor_valid = false;
    context->smart_target_cycle = 0;
    context->semantic_storage = std::move(loaded->snapshot);
    context->semantic_regions = &context->semantic_storage;

    if (!loaded->semantic_error.empty()) {
        std::cerr << "[Screenshot] Semantic smart targets degraded: "
                  << loaded->semantic_error << '\n';
    }
    if (!loaded->content_error.empty()) {
        std::cerr << "[Screenshot] Content smart targets unavailable: "
                  << loaded->content_error << '\n';
    }

    if (!context->selecting && !ocr_mode_active(*context)) {
        if (!loaded->semantic_error.empty() && !loaded->content_error.empty()) {
            show_transient_error(
                context,
                "Smart targeting limited · manual capture still works"
            );
        } else if (!loaded->content_error.empty()) {
            show_transient_error(
                context,
                "Content targeting unavailable · window/manual capture still works"
            );
        } else if (!loaded->semantic_error.empty()) {
            show_transient_error(
                context,
                "Window/layer targeting degraded · content/manual capture still works"
            );
        }
    }

    print_smart_target_summary(context->semantic_storage, context->monitor.connector);
    start_semantic_reveal(context);

    if (screenshot_timing_enabled()) {
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - context->process_start
        ).count();
        std::cerr << "[Screenshot timing] semantic targets "
                  << loaded->semantic_ms << " ms\n";
        std::cerr << "[Screenshot timing] content worker "
                  << loaded->content_ms << " ms\n";
        std::cerr << "[Screenshot timing] smart targets visible by "
                  << elapsed << " ms\n";
    }

    queue_canvas(context);
    return G_SOURCE_REMOVE;
}

void start_smart_region_detection(OverlayContext* context) {
    if (context == nullptr || context->shutting_down || context->frame == nullptr ||
        context->detection_started) return;
    context->detection_started = true;

    const FrozenFrame* frame = context->frame;
    const MonitorTarget monitor = context->monitor;
    DetectionState* state = &context->detection_state;

    context->detection_state.cancel_requested.store(false, std::memory_order_release);
    try {
        context->detection_thread = std::thread([state, frame, monitor]() {
            SmartRegionLoadResult loaded;
            try {
                loaded = SmartRegionLoader::load(
                    monitor,
                    *frame,
                    &state->cancel_requested
                );
            } catch (const std::exception& exception) {
                loaded.snapshot.monitor_width = monitor.logical_width;
                loaded.snapshot.monitor_height = monitor.logical_height;
                loaded.semantic_error = std::string{"smart-target worker exception: "} +
                    exception.what();
                loaded.content_error = loaded.semantic_error;
            } catch (...) {
                loaded.snapshot.monitor_width = monitor.logical_width;
                loaded.snapshot.monitor_height = monitor.logical_height;
                loaded.semantic_error = "smart-target worker failed with an unknown exception";
                loaded.content_error = loaded.semantic_error;
            }
            {
                std::lock_guard lock(state->mutex);
                state->result = std::move(loaded);
            }
            state->ready.store(true, std::memory_order_release);
        });
    } catch (const std::exception& exception) {
        context->detection_started = false;
        context->detection_state.cancel_requested.store(false, std::memory_order_release);
        std::cerr << "[Screenshot] Unable to start smart-target worker: "
                  << exception.what() << '\n';
        show_transient_error(
            context,
            "Smart targeting unavailable · manual capture still works"
        );
        return;
    }

    context->detection_poll_source = g_timeout_add(
        8,
        poll_smart_region_detection,
        context
    );
    if (context->detection_poll_source == 0) {
        context->detection_state.cancel_requested.store(true, std::memory_order_release);
        context->detection_thread.join();
        context->detection_state.ready.store(false, std::memory_order_release);
        {
            std::lock_guard lock(context->detection_state.mutex);
            context->detection_state.result.reset();
        }
        context->detection_state.cancel_requested.store(false, std::memory_order_release);
        context->detection_started = false;
        show_transient_error(
            context,
            "Smart targeting unavailable · manual capture still works"
        );
    }
}

void on_window_map(GtkWidget*, gpointer user_data) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->shutting_down) return;

    if (screenshot_timing_enabled()) {
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - context->process_start
        ).count();
        std::cerr << "[Screenshot timing] overlay mapped " << elapsed << " ms\n";
    }

    const auto now = std::chrono::steady_clock::now();
    context->hint_full_until = now + kHintInitialHold;
    context->hint_fade_until = context->hint_full_until + kHintFadeDuration;
    animate_until(context, context->hint_fade_until);
    start_entry_sweep(context);
    update_interaction_cursor(context);

    // Nothing expensive runs before this point. The frozen screen is already
    // mapped; smart-target discovery can now consume CPU without delaying it.
    start_smart_region_detection(context);
}

void on_window_destroy(GtkWidget* widget, gpointer user_data) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context != nullptr && !context->shutting_down) {
        context->shutting_down = true;
        request_async_cancellation(context);

        // The overlay child can already be in destruction by the time the
        // window's destroy signal runs. GTK will remove its tick callback for
        // us, so never dereference the cached canvas from this teardown path.
        context->visual_tick_source = 0;
        context->canvas = nullptr;
        remove_async_sources(context);
        context->window = nullptr;
    }

    GtkApplication* app = gtk_window_get_application(GTK_WINDOW(widget));
    if (app != nullptr) g_application_quit(G_APPLICATION(app));
}

void install_overlay_css(GtkWidget* widget) {
    if (widget == nullptr) return;

    GtkCssProvider* provider = gtk_css_provider_new();
    const char* css = R"CSS(
        .rh-shot-toolbar {
            background: rgba(12, 12, 16, 0.66);
            border: 1px solid rgba(255, 255, 255, 0.10);
            border-radius: 999px;
            padding: 7px;
            box-shadow: 0 12px 28px rgba(0, 0, 0, 0.28);
        }

        .rh-shot-ratio-button {
            color: rgba(245, 245, 252, 0.94);
            background: transparent;
            border: none;
            border-radius: 999px;
            min-height: 32px;
            min-width: 58px;
            padding: 0 15px;
            font-weight: 700;
        }

        .rh-shot-ratio-button:hover {
            background: rgba(255, 255, 255, 0.09);
        }

        .rh-shot-ratio-button:checked {
            color: rgba(18, 16, 10, 0.98);
            background: linear-gradient(to bottom, rgba(246, 220, 128, 0.99), rgba(236, 193, 94, 0.99));
        }
    )CSS";
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        gtk_widget_get_display(widget),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);
}

void activate(GtkApplication* app, gpointer user_data) {
    auto* context = static_cast<OverlayContext*>(user_data);
    if (context == nullptr || context->frame == nullptr) {
        g_application_quit(G_APPLICATION(app));
        return;
    }

    const FrozenFrame& frame = *context->frame;
    if (frame.width <= 0 || frame.height <= 0 || frame.stride <= 0 || frame.rgba.empty()) {
        std::cerr << "[Screenshot] Frozen Wayland frame is empty\n";
        g_application_quit(G_APPLICATION(app));
        return;
    }

    GBytes* bytes = g_bytes_new(frame.rgba.data(), frame.rgba.size());
    GdkTexture* texture = gdk_memory_texture_new(
        frame.width,
        frame.height,
        GDK_MEMORY_R8G8B8A8,
        bytes,
        static_cast<gsize>(frame.stride)
    );
    g_bytes_unref(bytes);
    if (texture == nullptr) {
        std::cerr << "[Screenshot] Unable to create texture from frozen Wayland frame\n";
        g_application_quit(G_APPLICATION(app));
        return;
    }

    GtkWidget* window_widget = gtk_application_window_new(app);
    context->window = GTK_WINDOW(window_widget);
    gtk_window_set_title(context->window, "Realmheart Screenshot");
    gtk_window_set_decorated(context->window, false);
    gtk_window_set_resizable(context->window, false);

    auto spec = realmheart::ui::make_layer_surface_spec(
        "selection",
        realmheart::ui::LayerSurfaceLevel::Overlay,
        realmheart::ui::LayerKeyboardMode::Exclusive
    );
    spec.anchor_left = true;
    spec.anchor_right = true;
    spec.anchor_top = true;
    spec.anchor_bottom = true;
    spec.exclusive_zone = -1;
    realmheart::ui::apply_layer_surface(context->window, spec);

    install_overlay_css(window_widget);

    GtkWidget* overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(overlay, true);
    gtk_widget_set_vexpand(overlay, true);

    GtkWidget* picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
    g_object_unref(texture);
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_FILL);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), true);
    gtk_widget_set_hexpand(picture, true);
    gtk_widget_set_vexpand(picture, true);
    gtk_widget_set_can_target(picture, false);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), picture);

    GtkWidget* canvas = gtk_drawing_area_new();
    context->canvas = canvas;
    gtk_widget_set_hexpand(canvas, true);
    gtk_widget_set_vexpand(canvas, true);
    gtk_widget_set_can_target(canvas, true);
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(canvas),
        draw_overlay,
        context,
        nullptr
    );
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), canvas);

    GtkWidget* ratio_toolbar = make_ratio_toolbar(context);
    context->ratio_toolbar = ratio_toolbar;
    if (ratio_toolbar != nullptr) {
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), ratio_toolbar);
    }

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_motion_enter), context);
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), context);
    g_signal_connect(motion, "leave", G_CALLBACK(on_motion_leave), context);
    gtk_widget_add_controller(canvas, motion);

    GtkEventController* scroll = gtk_event_controller_scroll_new(
        static_cast<GtkEventControllerScrollFlags>(
            GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
            GTK_EVENT_CONTROLLER_SCROLL_DISCRETE
        )
    );
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), context);
    gtk_widget_add_controller(canvas, scroll);

    GtkGesture* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), context);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), context);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), context);
    gtk_widget_add_controller(canvas, GTK_EVENT_CONTROLLER(drag));

    GtkGesture* ocr_drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(ocr_drag), GDK_BUTTON_SECONDARY);
    g_signal_connect(ocr_drag, "drag-begin", G_CALLBACK(on_ocr_drag_begin), context);
    g_signal_connect(ocr_drag, "drag-update", G_CALLBACK(on_ocr_drag_update), context);
    g_signal_connect(ocr_drag, "drag-end", G_CALLBACK(on_ocr_drag_end), context);
    gtk_widget_add_controller(canvas, GTK_EVENT_CONTROLLER(ocr_drag));

    gtk_window_set_child(context->window, overlay);

    GtkEventController* key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(on_key_pressed), context);
    gtk_widget_add_controller(window_widget, key_controller);

    g_signal_connect(window_widget, "map", G_CALLBACK(on_window_map), context);
    g_signal_connect(window_widget, "destroy", G_CALLBACK(on_window_destroy), context);

    // Bind the layer to the resolved connector before its first visible commit.
    gtk_widget_realize(window_widget);
    if (GdkMonitor* monitor = find_monitor_by_connector(
            window_widget,
            context->monitor.connector
        )) {
        gtk_layer_set_monitor(context->window, monitor);
        g_object_unref(monitor);
    } else {
        std::cerr << "[Screenshot] GDK could not resolve monitor connector '"
                  << context->monitor.connector
                  << "'; compositor monitor selection will be used as fallback\n";
    }

    gtk_widget_set_cursor_from_name(canvas, "crosshair");
    gtk_window_present(context->window);
}

} // namespace

int ScreenshotOverlay::run(
    const FrozenFrame& frozen_frame,
    const MonitorTarget& monitor,
    std::chrono::steady_clock::time_point process_start
) {
    OverlayContext context;
    context.frame = &frozen_frame;
    context.monitor = monitor;
    context.process_start = process_start;
    context.semantic_storage.monitor_width = monitor.logical_width;
    context.semantic_storage.monitor_height = monitor.logical_height;
    context.semantic_regions = &context.semantic_storage;

    GtkApplication* app = gtk_application_new(
        "dev.realmheart.Screenshot",
        G_APPLICATION_NON_UNIQUE
    );
    g_signal_connect(app, "activate", G_CALLBACK(activate), &context);

    const int status = g_application_run(G_APPLICATION(app), 0, nullptr);

    request_async_cancellation(&context);
    remove_async_sources(&context);
    if (context.detection_thread.joinable()) {
        context.detection_thread.join();
    }
    if (context.ocr_thread.joinable()) {
        context.ocr_thread.join();
    }

    g_object_unref(app);
    return status;
}

} // namespace realmheart::screenshot
