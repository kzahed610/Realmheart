#include "ui/bar/widgets/BatteryWidget.hpp"

#include "ui/bar/widgets/BatteryWidgetModel.hpp"
#include "ui/bar/widgets/PopoverReveal.hpp"

#include <iomanip>
#include <sstream>
#include <initializer_list>
#include <string>
#include <utility>

namespace realmheart::ui::bar::widgets {
namespace {

constexpr int kPopoverOffsetX = 9;
constexpr int kPopoverOffsetY = -5;

} // namespace

BatteryWidget::BatteryWidget(
    std::function<void(GtkPopover*)> request_exclusive_open,
    std::function<void()> request_battery_refresh
) : request_exclusive_open_(std::move(request_exclusive_open)),
    request_battery_refresh_(std::move(request_battery_refresh)),
    button_(
        "Realmheart-Icons/battery.svg",
        "Bt",
        "Hold for battery details"
    ) {
    button_.add_css_class("realmheart-battery-button");

    popover_ = gtk_popover_new();
    gtk_widget_add_css_class(popover_, "realmheart-bar-popover");
    gtk_widget_add_css_class(popover_, "realmheart-battery-popover");
    gtk_widget_add_css_class(popover_, "realmheart-compact-popover");
    gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_RIGHT);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(popover_), FALSE);
    gtk_popover_set_offset(GTK_POPOVER(popover_), kPopoverOffsetX, kPopoverOffsetY);
    gtk_widget_set_parent(popover_, button_.button());

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_size_request(root, 154, -1);
    percentage_label_ = gtk_label_new("Battery unavailable");
    gtk_widget_add_css_class(percentage_label_, "realmheart-battery-percentage");
    gtk_label_set_xalign(GTK_LABEL(percentage_label_), 0.0F);
    state_label_ = gtk_label_new("");
    gtk_widget_add_css_class(state_label_, "realmheart-popover-muted");
    gtk_label_set_xalign(GTK_LABEL(state_label_), 0.0F);
    rate_label_ = gtk_label_new("");
    gtk_widget_add_css_class(rate_label_, "realmheart-battery-rate");
    gtk_label_set_xalign(GTK_LABEL(rate_label_), 0.0F);
    gtk_label_set_wrap(GTK_LABEL(rate_label_), TRUE);
    gtk_box_append(GTK_BOX(root), percentage_label_);
    gtk_box_append(GTK_BOX(root), state_label_);
    gtk_box_append(GTK_BOX(root), rate_label_);
    gtk_popover_set_child(GTK_POPOVER(popover_), root);

    GtkGesture* hold = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(hold), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(hold), GTK_PHASE_CAPTURE
    );
    g_signal_connect(hold, "pressed", G_CALLBACK(+[](
        GtkGestureClick*, int, double, double, gpointer data
    ) {
        static_cast<BatteryWidget*>(data)->show_held();
    }), this);
    g_signal_connect(hold, "released", G_CALLBACK(+[](
        GtkGestureClick*, int, double, double, gpointer data
    ) {
        static_cast<BatteryWidget*>(data)->hide_held();
    }), this);
    g_signal_connect(hold, "cancel", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer data) {
        static_cast<BatteryWidget*>(data)->hide_held();
    }), this);
    gtk_widget_add_controller(button_.button(), GTK_EVENT_CONTROLLER(hold));

    update(std::nullopt);
}

BatteryWidget::~BatteryWidget() {
    if (popover_ != nullptr && gtk_widget_get_parent(popover_) != nullptr) {
        gtk_widget_unparent(popover_);
    }
}

void BatteryWidget::set_layout(const BarGeometry& geometry) {
    button_.set_layout(geometry.icon_button_extent, geometry.icon_size);
}

void BatteryWidget::update(const std::optional<services::BatteryStatus>& status) {
    status_ = status;
    for (const char* css_class : {
        "realmheart-battery-charging",
        "realmheart-battery-critical",
        "realmheart-battery-low",
        "realmheart-battery-normal",
    }) {
        button_.remove_css_class(css_class);
    }

    if (!status_) {
        button_.set_icon("Realmheart-Icons/battery.svg", "Bt");
        button_.set_enabled(false);
        button_.set_tooltip("Battery unavailable");
        update_popup();
        return;
    }

    button_.set_icon(battery_icon_path(*status_), "Bt");
    button_.set_enabled(true);
    if (status_->charging) {
        button_.add_css_class("realmheart-battery-charging");
    } else if (status_->percentage <= 15) {
        button_.add_css_class("realmheart-battery-critical");
    } else if (status_->percentage <= 35) {
        button_.add_css_class("realmheart-battery-low");
    } else {
        button_.add_css_class("realmheart-battery-normal");
    }
    button_.set_tooltip(
        "Battery: " + std::to_string(status_->percentage) + "% (" + status_->status + ")"
    );
    update_popup();
}

void BatteryWidget::update_popup() {
    if (!status_) {
        gtk_label_set_text(GTK_LABEL(percentage_label_), "Battery unavailable");
        gtk_label_set_text(GTK_LABEL(state_label_), "");
        gtk_label_set_text(GTK_LABEL(rate_label_), "");
        return;
    }

    const std::string percentage = std::to_string(status_->percentage) + "%";
    gtk_label_set_text(GTK_LABEL(percentage_label_), percentage.c_str());
    gtk_label_set_text(GTK_LABEL(state_label_), status_->status.c_str());

    std::ostringstream details;
    if (status_->charging) {
        if (status_->time_to_full_minutes) {
            details << "Estimated time to full  ~"
                    << format_battery_duration(*status_->time_to_full_minutes);
        } else {
            details << "External power connected";
        }
    } else {
        if (status_->time_remaining_minutes) {
            details << "Estimated time left  ~"
                    << format_battery_duration(*status_->time_remaining_minutes);
        } else {
            details << "Estimated time left unavailable";
        }

        if (status_->rate_watts) {
            details << "\nDischarge rate  " << std::fixed << std::setprecision(2)
                    << *status_->rate_watts << " W";
        } else {
            details << "\nDischarge rate unavailable";
        }
    }
    gtk_label_set_text(GTK_LABEL(rate_label_), details.str().c_str());
}

void BatteryWidget::show_held() {
    if (request_battery_refresh_) request_battery_refresh_();
    update_popup();
    if (request_exclusive_open_) request_exclusive_open_(GTK_POPOVER(popover_));
    reveal_popover(GTK_POPOVER(popover_), kPopoverOffsetX, kPopoverOffsetY);
}

void BatteryWidget::hide_held() {
    gtk_popover_popdown(GTK_POPOVER(popover_));
}

void BatteryWidget::close() {
    hide_held();
}

} // namespace realmheart::ui::bar::widgets
