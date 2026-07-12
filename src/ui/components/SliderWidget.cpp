#include "ui/components/SliderWidget.hpp"

#include <utility>

namespace realmheart::ui::components {

SliderWidget::SliderWidget(
    std::string label,
    double min,
    double max,
    double initial,
    std::function<std::optional<double>(double)> on_change
) : on_change_(std::move(on_change)),
    pending_value_(initial),
    confirmed_value_(initial) {
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(box_, "realmheart-module-row");
    gtk_widget_add_css_class(box_, "realmheart-module-slider");
    gtk_widget_set_margin_start(box_, 12);
    gtk_widget_set_margin_end(box_, 12);
    gtk_widget_set_margin_top(box_, 6);
    gtk_widget_set_margin_bottom(box_, 6);

    GtkWidget* name = gtk_label_new(label.c_str());
    gtk_label_set_xalign(GTK_LABEL(name), 0.0F);
    gtk_box_append(GTK_BOX(box_), name);

    scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1.0);
    gtk_range_set_value(GTK_RANGE(scale_), initial);
    value_changed_handler_ = g_signal_connect(
        scale_,
        "value-changed",
        G_CALLBACK(+[](GtkRange* range, gpointer data) {
            auto* self = static_cast<SliderWidget*>(data);
            if (self->updating_) return;

            self->pending_value_ = gtk_range_get_value(range);
            if (self->debounce_source_ != 0) {
                g_source_remove(self->debounce_source_);
            }
            self->debounce_source_ = g_timeout_add(120, +[](gpointer callback_data) -> gboolean {
                auto* widget = static_cast<SliderWidget*>(callback_data);
                widget->debounce_source_ = 0;

                const auto actual = widget->on_change_
                    ? widget->on_change_(widget->pending_value_)
                    : std::optional<double>{widget->pending_value_};
                if (actual) widget->confirmed_value_ = *actual;

                widget->updating_ = true;
                gtk_range_set_value(GTK_RANGE(widget->scale_), widget->confirmed_value_);
                widget->updating_ = false;
                return G_SOURCE_REMOVE;
            }, self);
        }),
        this
    );
    gtk_box_append(GTK_BOX(box_), scale_);
}

SliderWidget::~SliderWidget() {
    if (debounce_source_ != 0) {
        g_source_remove(debounce_source_);
        debounce_source_ = 0;
    }
    if (scale_ != nullptr && value_changed_handler_ != 0) {
        g_signal_handler_disconnect(scale_, value_changed_handler_);
    }
}

GtkWidget* SliderWidget::get_widget() {
    return box_;
}

void SliderWidget::set_value(double value) {
    confirmed_value_ = value;
    pending_value_ = value;
    updating_ = true;
    gtk_range_set_value(GTK_RANGE(scale_), value);
    updating_ = false;
}

} // namespace realmheart::ui::components
