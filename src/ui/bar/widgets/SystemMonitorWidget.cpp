#include "ui/bar/widgets/SystemMonitorWidget.hpp"

#include "core/TaskExecutor.hpp"

#include <algorithm>
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



} // namespace

SystemMonitorWidget::SystemMonitorWidget(
    std::function<void(GtkPopover*)> request_exclusive_open
) : request_exclusive_open_(std::move(request_exclusive_open)),
    button_(
        "Realmheart-Icons/system/realmforge-settings.svg",
        "Sy",
        "Hold for system usage"
    ) {
    button_.add_css_class("realmheart-system-monitor-button");
    async_state_->owner = this;

    popover_ = gtk_popover_new();
    gtk_widget_add_css_class(popover_, "realmheart-bar-popover");
    gtk_widget_add_css_class(popover_, "realmheart-system-monitor-popover");
    gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_RIGHT);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(popover_), FALSE);
    gtk_popover_set_offset(GTK_POPOVER(popover_), 9, -6);
    gtk_widget_set_parent(popover_, button_.button());

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_set_size_request(root, 255, -1);
    GtkWidget* title = gtk_label_new("System usage");
    gtk_widget_add_css_class(title, "realmheart-popover-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
    gtk_box_append(GTK_BOX(root), title);

    state_label_ = gtk_label_new("Hold to sample");
    gtk_widget_add_css_class(state_label_, "realmheart-popover-muted");
    gtk_label_set_xalign(GTK_LABEL(state_label_), 0.0F);
    gtk_box_append(GTK_BOX(root), state_label_);

    cpu_ = add_row(root, "CPU");
    memory_ = add_row(root, "RAM");
    swap_ = add_row(root, "SWAP");
    gpu_ = add_row(root, "GPU / iGPU");
    gtk_popover_set_child(GTK_POPOVER(popover_), root);

    GtkGesture* hold = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(hold), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(hold), GTK_PHASE_CAPTURE
    );
    g_signal_connect(hold, "pressed", G_CALLBACK(+[](
        GtkGestureClick*, int, double, double, gpointer data
    ) {
        static_cast<SystemMonitorWidget*>(data)->show_held();
    }), this);
    g_signal_connect(hold, "released", G_CALLBACK(+[](
        GtkGestureClick*, int, double, double, gpointer data
    ) {
        static_cast<SystemMonitorWidget*>(data)->hide_held();
    }), this);
    g_signal_connect(hold, "cancel", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer data) {
        static_cast<SystemMonitorWidget*>(data)->hide_held();
    }), this);
    gtk_widget_add_controller(button_.button(), GTK_EVENT_CONTROLLER(hold));
}

SystemMonitorWidget::~SystemMonitorWidget() {
    if (refresh_timer_id_ != 0) {
        g_source_remove(refresh_timer_id_);
        refresh_timer_id_ = 0;
    }
    async_state_->alive = false;
    async_state_->owner = nullptr;
    if (popover_ != nullptr && gtk_widget_get_parent(popover_) != nullptr) {
        gtk_widget_unparent(popover_);
    }
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

void SystemMonitorWidget::show_held() {
    held_ = true;
    if (request_exclusive_open_) request_exclusive_open_(GTK_POPOVER(popover_));
    gtk_label_set_text(GTK_LABEL(state_label_), "Measuring…");
    gtk_popover_popup(GTK_POPOVER(popover_));
    request_sample();

    if (refresh_timer_id_ == 0) {
        refresh_timer_id_ = g_timeout_add(750, +[](gpointer data) -> gboolean {
            auto* self = static_cast<SystemMonitorWidget*>(data);
            if (!self->held_) {
                self->refresh_timer_id_ = 0;
                return G_SOURCE_REMOVE;
            }
            self->request_sample();
            return G_SOURCE_CONTINUE;
        }, this);
    }
}

void SystemMonitorWidget::hide_held() {
    held_ = false;
    if (refresh_timer_id_ != 0) {
        g_source_remove(refresh_timer_id_);
        refresh_timer_id_ = 0;
    }
    gtk_popover_popdown(GTK_POPOVER(popover_));
}

void SystemMonitorWidget::close() {
    hide_held();
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
    gtk_label_set_text(GTK_LABEL(state_label_), held_ ? "Live while held" : "Last sample");

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
