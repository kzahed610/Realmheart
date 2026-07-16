#include "ui/bar/widgets/SystemMonitorWidget.hpp"

#include "ui/bar/widgets/AttachedPopover.hpp"
#include "core/TaskExecutor.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/bar/widgets/PopoverReveal.hpp"

#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace realmheart::ui::bar::widgets {
namespace {

std::string percent_text(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(value < 10.0 ? 1 : 0) << value << '%';
    return out.str();
}

std::string frequency_text(double frequency_mhz) {
    std::ostringstream out;
    if (frequency_mhz >= 1000.0) {
        out << std::fixed << std::setprecision(2) << (frequency_mhz / 1000.0) << " GHz";
    } else {
        out << std::fixed << std::setprecision(0) << frequency_mhz << " MHz";
    }
    return out.str();
}

std::string memory_text(
    std::uint64_t used_kib,
    std::uint64_t total_kib,
    double percent
) {
    if (total_kib == 0) return "Not configured";

    constexpr double kib_per_gib = 1024.0 * 1024.0;
    constexpr double kib_per_mib = 1024.0;
    const bool use_gib = total_kib >= static_cast<std::uint64_t>(kib_per_gib);
    const double divisor = use_gib ? kib_per_gib : kib_per_mib;

    std::ostringstream out;
    out << std::fixed << std::setprecision(use_gib ? 1 : 0)
        << (static_cast<double>(used_kib) / divisor)
        << " / "
        << (static_cast<double>(total_kib) / divisor)
        << (use_gib ? " GiB" : " MiB")
        << " · "
        << percent_text(percent);
    return out.str();
}

std::string processor_text(double percent, const std::optional<double>& frequency_mhz) {
    std::string text = percent_text(percent);
    if (frequency_mhz) text += " · " + frequency_text(*frequency_mhz);
    return text;
}

// Preserve the exact native-popover alignment: its right-side offset was five
// pixels from the system pill, and its vertical centre followed the pill.
constexpr int kSystemLayerExtraOffsetX = kExpandingPopoverOffsetX;
constexpr int kSystemLayerFallbackLeft = 56;
constexpr int kSystemLayerFallbackTop = 92;
constexpr double kSystemRevealDurationUs = 210000.0;

} // namespace

SystemMonitorWidget::SystemMonitorWidget(
    GtkApplication* app,
    std::function<void()> request_exclusive_open
) : request_exclusive_open_(std::move(request_exclusive_open)) {
    async_state_->owner = this;

    button_ = gtk_button_new();
    gtk_widget_add_css_class(button_, "realmheart-system-monitor-pill");
    gtk_widget_set_halign(button_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(button_, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(button_, "System usage: CPU, RAM, GPU");

    GtkWidget* metrics = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class(metrics, "realmheart-system-monitor-metrics");
    gtk_widget_set_halign(metrics, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(metrics, GTK_ALIGN_CENTER);

    const std::array metric_specs{
        std::pair{"Realmheart-Icons/cpu.svg", "realmheart-system-monitor-cpu"},
        std::pair{"Realmheart-Icons/ram.svg", "realmheart-system-monitor-ram"},
        std::pair{"Realmheart-Icons/gpu.svg", "realmheart-system-monitor-gpu"},
    };
    metric_icons_.reserve(metric_specs.size());
    for (const auto& [path, css_class] : metric_specs) {
        auto icon = std::make_unique<ThemedSvgIcon>(path, 20);
        icon->add_css_class("realmheart-system-monitor-metric");
        icon->add_css_class(css_class);
        gtk_box_append(GTK_BOX(metrics), icon->widget());
        metric_icons_.push_back(std::move(icon));
    }
    gtk_button_set_child(GTK_BUTTON(button_), metrics);

    // Match the media panel's successful architecture: a transparent overlay
    // layer surface whose custom Cairo shell is the only painted background.
    // The position is derived from the system pill's actual allocation so the
    // existing final geometry is preserved rather than retuned by hand.
    layer_window_ = app != nullptr
        ? gtk_application_window_new(app)
        : gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(layer_window_), "Realmheart System Usage");
    gtk_window_set_decorated(GTK_WINDOW(layer_window_), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(layer_window_), FALSE);
    gtk_widget_add_css_class(layer_window_, "realmheart-system-monitor-layer-window");
    gtk_widget_remove_css_class(layer_window_, "background");

    realmheart::ui::LayerSurfaceSpec system_surface;
    system_surface.surface_namespace = "realmheart-system-monitor";
    system_surface.layer = realmheart::ui::LayerSurfaceLevel::Overlay;
    system_surface.keyboard_mode = realmheart::ui::LayerKeyboardMode::None;
    system_surface.anchor_left = true;
    system_surface.anchor_top = true;
    system_surface.exclusive_zone = 0;
    realmheart::ui::apply_layer_surface(GTK_WINDOW(layer_window_), system_surface);

    // Align from the physical monitor origin, not from the usable area already
    // shifted by the vertical bar's exclusive zone.
    gtk_layer_set_exclusive_zone(GTK_WINDOW(layer_window_), -1);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_set_size_request(root, 255, -1);
    GtkWidget* title = gtk_label_new("System usage");
    gtk_widget_add_css_class(title, "realmheart-popover-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
    gtk_box_append(GTK_BOX(root), title);

    state_label_ = gtk_label_new("Live while open");
    gtk_widget_add_css_class(state_label_, "realmheart-popover-muted");
    gtk_label_set_xalign(GTK_LABEL(state_label_), 0.0F);
    gtk_box_append(GTK_BOX(root), state_label_);

    cpu_ = add_row(root, "CPU");
    memory_ = add_row(root, "RAM");
    swap_ = add_row(root, "SWAP");
    gpu_ = add_row(root, "GPU / iGPU");

    // Standard expanding geometry: no media-only top flush or screen hug.
    layer_shell_ = create_expanding_popover_shell(root);
    gtk_window_set_child(GTK_WINDOW(layer_window_), layer_shell_);

    g_signal_connect(button_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<SystemMonitorWidget*>(data);
        self->trigger_click_feedback();
        self->toggle();
    }), this);
}

SystemMonitorWidget::~SystemMonitorWidget() {
    hide_layer_window();
    if (click_feedback_timer_id_ != 0) {
        g_source_remove(click_feedback_timer_id_);
        click_feedback_timer_id_ = 0;
    }
    async_state_->alive = false;
    async_state_->owner = nullptr;
    if (layer_window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(layer_window_));
        layer_window_ = nullptr;
        layer_shell_ = nullptr;
    }
}

void SystemMonitorWidget::trigger_click_feedback() {
    if (button_ == nullptr) return;
    gtk_widget_add_css_class(button_, "realmheart-click-feedback");
    if (click_feedback_timer_id_ != 0) g_source_remove(click_feedback_timer_id_);
    click_feedback_timer_id_ = g_timeout_add(130, +[](gpointer data) -> gboolean {
        auto* self = static_cast<SystemMonitorWidget*>(data);
        self->click_feedback_timer_id_ = 0;
        if (self->button_ != nullptr) {
            gtk_widget_remove_css_class(self->button_, "realmheart-click-feedback");
        }
        return G_SOURCE_REMOVE;
    }, this);
}

SystemMonitorWidget::UsageRow SystemMonitorWidget::add_row(GtkWidget* parent, const char* name) {
    GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget* label = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    GtkWidget* value = gtk_label_new("—");
    gtk_widget_add_css_class(value, "realmheart-system-usage-value");
    gtk_label_set_xalign(GTK_LABEL(value), 0.0F);
    gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);
    GtkWidget* progress = gtk_progress_bar_new();
    gtk_widget_add_css_class(progress, "realmheart-system-usage-bar");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress), 0.0);

    gtk_box_append(GTK_BOX(header), label);
    gtk_box_append(GTK_BOX(header), value);
    gtk_box_append(GTK_BOX(row), header);
    gtk_box_append(GTK_BOX(row), progress);
    gtk_box_append(GTK_BOX(parent), row);
    return {value, progress};
}

int SystemMonitorWidget::layer_left_margin() const {
    GtkWidget* bar_window = button_ != nullptr
        ? gtk_widget_get_ancestor(button_, GTK_TYPE_WINDOW)
        : nullptr;
    if (button_ == nullptr || bar_window == nullptr) return kSystemLayerFallbackLeft;

    graphene_rect_t bounds{};
    if (!gtk_widget_compute_bounds(button_, bar_window, &bounds)) {
        return kSystemLayerFallbackLeft;
    }

    return std::max(
        0,
        static_cast<int>(std::lround(bounds.origin.x + bounds.size.width))
            + kSystemLayerExtraOffsetX
    );
}

int SystemMonitorWidget::layer_top_margin() const {
    GtkWidget* bar_window = button_ != nullptr
        ? gtk_widget_get_ancestor(button_, GTK_TYPE_WINDOW)
        : nullptr;
    if (button_ == nullptr || bar_window == nullptr || layer_shell_ == nullptr) {
        return kSystemLayerFallbackTop;
    }

    graphene_rect_t bounds{};
    if (!gtk_widget_compute_bounds(button_, bar_window, &bounds)) {
        return kSystemLayerFallbackTop;
    }

    int minimum_width = 0;
    int natural_width = 0;
    gtk_widget_measure(
        layer_shell_,
        GTK_ORIENTATION_HORIZONTAL,
        -1,
        &minimum_width,
        &natural_width,
        nullptr,
        nullptr
    );

    int minimum_height = 0;
    int natural_height = 0;
    gtk_widget_measure(
        layer_shell_,
        GTK_ORIENTATION_VERTICAL,
        std::max(minimum_width, natural_width),
        &minimum_height,
        &natural_height,
        nullptr,
        nullptr
    );

    const int shell_height = std::max(minimum_height, natural_height);
    if (shell_height <= 0) return kSystemLayerFallbackTop;

    const double anchor_center_y = bounds.origin.y + (bounds.size.height / 2.0);
    return std::max(
        0,
        static_cast<int>(std::lround(anchor_center_y - (shell_height / 2.0)))
    );
}

void SystemMonitorWidget::show_layer_window() {
    if (layer_window_ == nullptr) return;

    if (reveal_tick_id_ != 0) {
        gtk_widget_remove_tick_callback(layer_window_, reveal_tick_id_);
        reveal_tick_id_ = 0;
    }

    reveal_target_x_ = layer_left_margin();
    reveal_target_y_ = layer_top_margin();
    reveal_start_x_ = std::max(0, reveal_target_x_ - kExpandingPopoverRevealPixels);
    reveal_started_us_ = 0;

    gtk_widget_set_opacity(layer_window_, 0.0);
    gtk_layer_set_margin(
        GTK_WINDOW(layer_window_),
        GTK_LAYER_SHELL_EDGE_LEFT,
        reveal_start_x_
    );
    gtk_layer_set_margin(
        GTK_WINDOW(layer_window_),
        GTK_LAYER_SHELL_EDGE_TOP,
        reveal_target_y_
    );
    gtk_window_present(GTK_WINDOW(layer_window_));

    reveal_tick_id_ = gtk_widget_add_tick_callback(
        layer_window_,
        +[](GtkWidget* widget, GdkFrameClock* frame_clock, gpointer data) -> gboolean {
            auto* self = static_cast<SystemMonitorWidget*>(data);
            if (!gtk_widget_get_visible(widget)) {
                self->reveal_tick_id_ = 0;
                return G_SOURCE_REMOVE;
            }

            const gint64 now = gdk_frame_clock_get_frame_time(frame_clock);
            if (self->reveal_started_us_ == 0) self->reveal_started_us_ = now;

            const double linear = std::clamp(
                static_cast<double>(now - self->reveal_started_us_) /
                    kSystemRevealDurationUs,
                0.0,
                1.0
            );
            const double eased = 1.0 - std::pow(1.0 - linear, 3.0);
            const int left = self->reveal_start_x_ + static_cast<int>(std::lround(
                static_cast<double>(self->reveal_target_x_ - self->reveal_start_x_) * eased
            ));

            const double fade_delay = popover_fade_delay_after_travel(
                kExpandingPopoverRevealPixels,
                kExpandingPopoverHiddenTravelPixels
            );
            const double opacity_linear = std::clamp(
                (linear - fade_delay) / std::max(0.001, 1.0 - fade_delay),
                0.0,
                1.0
            );
            const double opacity = 1.0 - std::pow(1.0 - opacity_linear, 3.0);

            gtk_layer_set_margin(GTK_WINDOW(widget), GTK_LAYER_SHELL_EDGE_LEFT, left);
            gtk_widget_set_opacity(widget, opacity);

            if (linear >= 1.0) {
                self->reveal_tick_id_ = 0;
                gtk_layer_set_margin(
                    GTK_WINDOW(widget),
                    GTK_LAYER_SHELL_EDGE_LEFT,
                    self->reveal_target_x_
                );
                gtk_widget_set_opacity(widget, 1.0);
                return G_SOURCE_REMOVE;
            }
            return G_SOURCE_CONTINUE;
        },
        this,
        nullptr
    );
}

void SystemMonitorWidget::hide_layer_window() {
    if (reveal_tick_id_ != 0 && layer_window_ != nullptr) {
        gtk_widget_remove_tick_callback(layer_window_, reveal_tick_id_);
        reveal_tick_id_ = 0;
    }
    reveal_started_us_ = 0;
    if (layer_window_ != nullptr) {
        gtk_widget_set_opacity(layer_window_, 1.0);
        gtk_widget_set_visible(layer_window_, FALSE);
    }
    open_ = false;
    stop_live_refresh();
}

void SystemMonitorWidget::toggle() {
    if (open_) {
        close();
        return;
    }
    open();
}

void SystemMonitorWidget::open() {
    open_ = true;
    if (request_exclusive_open_) request_exclusive_open_();
    gtk_label_set_text(GTK_LABEL(state_label_), "Measuring…");
    show_layer_window();
    request_sample();

    if (refresh_timer_id_ == 0) {
        refresh_timer_id_ = g_timeout_add(750, +[](gpointer data) -> gboolean {
            auto* self = static_cast<SystemMonitorWidget*>(data);
            if (!self->open_) {
                self->refresh_timer_id_ = 0;
                return G_SOURCE_REMOVE;
            }
            self->request_sample();
            return G_SOURCE_CONTINUE;
        }, this);
    }
}

void SystemMonitorWidget::stop_live_refresh() {
    if (refresh_timer_id_ != 0) {
        g_source_remove(refresh_timer_id_);
        refresh_timer_id_ = 0;
    }
}

void SystemMonitorWidget::close() {
    hide_layer_window();
}

void SystemMonitorWidget::request_sample() {
    if (async_state_->in_flight.exchange(true)) return;
    const auto state = async_state_;
    if (!realmheart::core::shared_task_executor().post([state] {
        auto snapshot = services::SystemMonitorService::read();
        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::optional<services::SystemUsageSnapshot> snapshot;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                payload->state->in_flight = false;
                if (payload->state->alive.load() && payload->state->owner != nullptr) {
                    payload->state->owner->apply(payload->snapshot);
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{state, std::move(snapshot)},
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    })) {
        async_state_->in_flight = false;
    }
}

void SystemMonitorWidget::apply(const std::optional<services::SystemUsageSnapshot>& snapshot) {
    if (!snapshot) {
        gtk_label_set_text(GTK_LABEL(state_label_), "Usage unavailable");
        return;
    }
    gtk_label_set_text(GTK_LABEL(state_label_), open_ ? "Live while open" : "Last sample");

    const auto set_row = [](const UsageRow& row, const std::string& text, double percent) {
        gtk_label_set_text(GTK_LABEL(row.value), text.c_str());
        gtk_progress_bar_set_fraction(
            GTK_PROGRESS_BAR(row.progress),
            std::clamp(percent / 100.0, 0.0, 1.0)
        );
    };

    set_row(
        cpu_,
        processor_text(snapshot->cpu_percent, snapshot->cpu_frequency_mhz),
        snapshot->cpu_percent
    );
    set_row(
        memory_,
        memory_text(
            snapshot->memory_used_kib,
            snapshot->memory_total_kib,
            snapshot->memory_percent
        ),
        snapshot->memory_percent
    );
    set_row(
        swap_,
        memory_text(
            snapshot->swap_used_kib,
            snapshot->swap_total_kib,
            snapshot->swap_percent
        ),
        snapshot->swap_percent
    );

    if (snapshot->gpu_percent) {
        set_row(
            gpu_,
            processor_text(*snapshot->gpu_percent, snapshot->gpu_frequency_mhz),
            *snapshot->gpu_percent
        );
    } else if (snapshot->gpu_frequency_mhz) {
        set_row(gpu_, frequency_text(*snapshot->gpu_frequency_mhz), 0.0);
    } else {
        set_row(gpu_, "Unavailable from kernel", 0.0);
    }
}

} // namespace realmheart::ui::bar::widgets
