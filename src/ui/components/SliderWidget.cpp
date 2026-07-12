#include "ui/components/SliderWidget.hpp"
#include <gtk/gtk.h>
#include <iostream>
#include <optional>

namespace realmheart::ui::components {

SliderWidget::SliderWidget(
    const std::string& label,
    double min,
    double max,
    double initial,
    std::function<std::optional<double>(double)> on_change
) : on_change_(on_change) {
    box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(box_, 12);
    gtk_widget_set_margin_end(box_, 12);
    gtk_widget_set_margin_top(box_, 6);
    gtk_widget_set_margin_bottom(box_, 6);

    GtkWidget* lbl_name = gtk_label_new(label.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
    gtk_box_append(GTK_BOX(box_), lbl_name);

    scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1.0);
    gtk_range_set_value(GTK_RANGE(scale_), initial);
    g_signal_connect(scale_, "value-changed", G_CALLBACK(+[](GtkRange* r, gpointer data) {
        auto* self = static_cast<SliderWidget*>(data);
        if (self->updating_) return;
        self->pending_value_ = gtk_range_get_value(r);
        if (self->debounce_source_ != 0) g_source_remove(self->debounce_source_);
        self->debounce_source_ = g_timeout_add(120, +[](gpointer callback_data) -> gboolean {
            auto* module = static_cast<SliderWidget*>(callback_data);
            module->debounce_source_ = 0;
            const auto actual = module->on_change_(module->pending_value_);
            if (actual) module->confirmed_value_ = *actual;

            module->updating_ = true;
            gtk_range_set_value(GTK_RANGE(module->scale_), module->confirmed_value_);
            module->updating_ = false;
            return G_SOURCE_REMOVE;
        }, self);
    }), this);
    gtk_box_append(GTK_BOX(box_), scale_);

    // Initialize provider once
    provider_ = gtk_css_provider_new();
    gtk_style_context_add_provider(gtk_widget_get_style_context(box_), 
                                   GTK_STYLE_PROVIDER(provider_), 
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

SliderWidget::~SliderWidget() = default;

GtkWidget* SliderWidget::get_widget() {
    return box_;
}

void SliderWidget::refresh() {
    // Sliders are typically driven by their own internal state or external set_value calls
}

void SliderWidget::set_value(double value) {
    updating_ = true;
    gtk_range_set_value(GTK_RANGE(scale_), value);
    updating_ = false;
}

void SliderWidget::apply_theme(const services::Palette& palette) {
    std::string text_color = palette.get("text", "#cdd6f4");
    
    std::string css = ".slider-widget { color: " + text_color + "; }";
    
    gtk_css_provider_load_from_string(provider_, css.c_str());
    
    gtk_widget_add_css_class(box_, "slider-widget");
}

} // namespace realmheart::ui::components
