#include "ui/components/LabelWidget.hpp"
#include <gtk/gtk.h>
#include <thread>
#include <iostream>

namespace realmheart::ui::components {

LabelWidget::LabelWidget(const std::string& label, const std::string& initial_value, Reader reader)
    : reader_(std::move(reader)) {
    
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(box_, 12);
    gtk_widget_set_margin_end(box_, 12);
    gtk_widget_set_margin_top(box_, 6);
    gtk_widget_set_margin_bottom(box_, 6);

    GtkWidget* lbl_name = gtk_label_new(label.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
    gtk_box_append(GTK_BOX(box_), lbl_name);

    val_label_ = gtk_label_new(initial_value.c_str());
    gtk_label_set_xalign(GTK_LABEL(val_label_), 1.0);
    gtk_box_append(GTK_BOX(box_), val_label_);

    // Initialize provider once
    provider_ = gtk_css_provider_new();
    gtk_style_context_add_provider(gtk_widget_get_style_context(box_), 
                                   GTK_STYLE_PROVIDER(provider_), 
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

GtkWidget* LabelWidget::get_widget() {
    return box_;
}

void LabelWidget::set_value(const std::string& value) {
    gtk_label_set_text(GTK_LABEL(val_label_), value.c_str());
}

void LabelWidget::refresh() {
    if (!reader_ || refresh_in_flight_.exchange(true)) return;

    const auto reader = reader_;
    std::thread([this, reader] {
        std::string value;
        try {
            value = reader();
        } catch (const std::exception&) {
            value = "Unavailable";
        }

        struct Result {
            LabelWidget* module;
            std::string value;
        };
        g_idle_add(+[](gpointer data) -> gboolean {
            std::unique_ptr<Result> result(static_cast<Result*>(data));
            if (result->module) {
                result->module->set_value(result->value);
                result->module->refresh_in_flight_ = false;
            }
            return G_SOURCE_REMOVE;
        }, new Result{this, std::move(value)});
    }).detach();
}

void LabelWidget::apply_theme(const services::Palette& palette) {
    std::string text_color = palette.get("text", "#cdd6f4");
    std::string accent_color = palette.get("accent", "#cba6f7");

    std::string css = ".label-widget { color: " + text_color + "; }\n"
                      ".label-widget-val { color: " + accent_color + "; font-weight: bold; }";
    
    gtk_css_provider_load_from_string(provider_, css.c_str());
    
    gtk_widget_add_css_class(box_, "label-widget");
    gtk_widget_add_css_class(val_label_, "label-widget-val");
}

} // namespace realmheart::ui::components
