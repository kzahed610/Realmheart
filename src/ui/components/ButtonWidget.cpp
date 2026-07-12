#include "ui/components/ButtonWidget.hpp"
#include <gtk/gtk.h>

namespace realmheart::ui::components {

ButtonWidget::ButtonWidget(const std::string& label, std::function<void()> on_click)
    : on_click_(std::move(on_click)) {
    
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(box_, 12);
    gtk_widget_set_margin_end(box_, 12);
    gtk_widget_set_margin_top(box_, 6);
    gtk_widget_set_margin_bottom(box_, 6);

    GtkWidget* lbl_name = gtk_label_new(label.c_str());
    gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
    gtk_box_append(GTK_BOX(box_), lbl_name);

    btn_ = gtk_button_new_with_label("Cycle");
    g_signal_connect(btn_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto* self = static_cast<ButtonWidget*>(data);
        self->on_click_();
        return FALSE;
    }), this);
    gtk_box_append(GTK_BOX(box_), btn_);

    // Initialize provider once
    provider_ = gtk_css_provider_new();
    gtk_style_context_add_provider(gtk_widget_get_style_context(box_), 
                                   GTK_STYLE_PROVIDER(provider_), 
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

GtkWidget* ButtonWidget::get_widget() {
    return box_;
}

void ButtonWidget::apply_theme(const services::Palette& palette) {
    std::string text_color = palette.get("text", "#cdd6f4");
    
    std::string css = ".button-widget { color: " + text_color + "; }";
    
    gtk_css_provider_load_from_string(provider_, css.c_str());
    
    gtk_widget_add_css_class(box_, "button-widget");
}

} // namespace realmheart::ui::components
