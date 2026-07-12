#include "ui/components/ButtonWidget.hpp"

#include <utility>

namespace realmheart::ui::components {

ButtonWidget::ButtonWidget(std::string label, std::function<void()> on_click)
    : on_click_(std::move(on_click)) {
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(box_, "realmheart-module-row");
    gtk_widget_add_css_class(box_, "realmheart-module-button");
    gtk_widget_set_margin_start(box_, 12);
    gtk_widget_set_margin_end(box_, 12);
    gtk_widget_set_margin_top(box_, 6);
    gtk_widget_set_margin_bottom(box_, 6);

    GtkWidget* name = gtk_label_new(label.c_str());
    gtk_label_set_xalign(GTK_LABEL(name), 0.0F);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_box_append(GTK_BOX(box_), name);

    button_ = gtk_button_new_with_label("Cycle");
    click_handler_ = g_signal_connect(
        button_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ButtonWidget*>(data);
            if (self->on_click_) self->on_click_();
        }),
        this
    );
    gtk_box_append(GTK_BOX(box_), button_);
}

ButtonWidget::~ButtonWidget() {
    if (button_ != nullptr && click_handler_ != 0) {
        g_signal_handler_disconnect(button_, click_handler_);
    }
}

GtkWidget* ButtonWidget::get_widget() {
    return box_;
}

} // namespace realmheart::ui::components
