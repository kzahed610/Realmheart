#include "ui/bar/widgets/BatteryWidget.hpp"

#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace realmheart::ui::bar::widgets {

BatteryWidget::BatteryWidget(
    std::function<void(GtkPopover*)> request_exclusive_open
) : request_exclusive_open_(std::move(request_exclusive_open)),
    button_(
        "Realmheart-Icons/battery/heartcore-medium.svg",
        "Bt",
        "Hold for battery details"
    ) {
    button_.add_css_class("realmheart-battery-button");

    popover_ = gtk_popover_new();
    gtk_widget_add_css_class(popover_, "realmheart-bar-popover");
    gtk_widget_add_css_class(popover_, "realmheart-battery-popover");
    gtk_popover_set_position(GTK_POPOVER(popover_), GTK_POS_RIGHT);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover_), TRUE);
    gtk_popover_set_autohide(GTK_POPOVER(popover_), FALSE);
    gtk_popover_set_offset(GTK_POPOVER(popover_), 9, -5);
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

const char* BatteryWidget::icon_for(const services::BatteryStatus& status) {
    if (status.charging) return "Realmheart-Icons/battery/heartcore-charging.svg";
    if (status.percentage <= 8) return "Realmheart-Icons/battery/heartcore-empty.svg";
    if (status.percentage <= 18) return "Realmheart-Icons/battery/heartcore-warning.svg";
    if (status.percentage <= 35) return "Realmheart-Icons/battery/heartcore-low.svg";
    if (status.percentage <= 62) return "Realmheart-Icons/battery/heartcore-medium.svg";
    if (status.percentage <= 88) return "Realmheart-Icons/battery/heartcore-high.svg";
    return "Realmheart-Icons/battery/heartcore-full.svg";
}

void BatteryWidget::update(const std::optional<services::BatteryStatus>& status) {
    status_ = status;
    if (!status_) {
        button_.set_enabled(false);
        button_.set_icon("Realmheart-Icons/battery/heartcore-empty.svg", "Bt");
        button_.set_tooltip("Battery unavailable");
        update_popup();
        return;
    }

    button_.set_enabled(true);
    button_.set_icon(icon_for(*status_), "Bt");
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

    if (!status_->charging && status_->rate_watts) {
        std::ostringstream rate;
        rate << "Discharge rate  " << std::fixed << std::setprecision(2)
             << *status_->rate_watts << " W";
        gtk_label_set_text(GTK_LABEL(rate_label_), rate.str().c_str());
    } else if (status_->charging) {
        gtk_label_set_text(GTK_LABEL(rate_label_), "External power connected");
    } else {
        gtk_label_set_text(GTK_LABEL(rate_label_), "Discharge rate unavailable");
    }
}

void BatteryWidget::show_held() {
    update_popup();
    if (request_exclusive_open_) request_exclusive_open_(GTK_POPOVER(popover_));
    gtk_popover_popup(GTK_POPOVER(popover_));
}

void BatteryWidget::hide_held() {
    gtk_popover_popdown(GTK_POPOVER(popover_));
}

void BatteryWidget::close() {
    hide_held();
}

} // namespace realmheart::ui::bar::widgets
