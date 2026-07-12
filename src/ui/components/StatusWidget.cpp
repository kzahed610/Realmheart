#include "ui/components/StatusWidget.hpp"

#include "ui/AssetResolver.hpp"

#include <utility>

namespace realmheart::ui::components {

StatusWidget::StatusWidget(Slot slot, std::function<void()> on_click)
    : slot_(std::move(slot)), on_click_(std::move(on_click)) {
    button_ = gtk_button_new();
    gtk_widget_add_css_class(button_, "realmheart-bar-status");
    gtk_widget_set_margin_top(button_, 4);
    gtk_widget_set_margin_bottom(button_, 4);

    overlay_ = gtk_overlay_new();
    if (const auto path = resolve_project_icon(slot_.icon_name)) {
        image_ = gtk_image_new_from_file(path->string().c_str());
        gtk_image_set_pixel_size(GTK_IMAGE(image_), 22);
        gtk_widget_set_size_request(image_, 22, 22);
    } else {
        image_ = gtk_label_new(slot_.fallback_text.c_str());
        gtk_widget_add_css_class(image_, "realmheart-bar-fallback-icon");
    }
    gtk_widget_set_halign(image_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(image_, GTK_ALIGN_CENTER);
    gtk_overlay_set_child(GTK_OVERLAY(overlay_), image_);
    gtk_button_set_child(GTK_BUTTON(button_), overlay_);

    click_handler_ = g_signal_connect(
        button_,
        "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<StatusWidget*>(data);
            if (self->on_click_) self->on_click_();
        }),
        this
    );

    set_status(slot_);
}

StatusWidget::~StatusWidget() {
    if (button_ != nullptr && click_handler_ != 0) {
        g_signal_handler_disconnect(button_, click_handler_);
    }
}

GtkWidget* StatusWidget::get_widget() {
    return button_;
}

void StatusWidget::update_badge() {
    if (slot_.badge_text.empty()) {
        if (badge_ != nullptr) gtk_widget_set_visible(badge_, FALSE);
        return;
    }

    if (badge_ == nullptr) {
        badge_ = gtk_label_new(slot_.badge_text.c_str());
        gtk_widget_add_css_class(badge_, "realmheart-bar-badge");
        gtk_widget_set_halign(badge_, GTK_ALIGN_END);
        gtk_widget_set_valign(badge_, GTK_ALIGN_START);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), badge_);
    } else {
        gtk_label_set_text(GTK_LABEL(badge_), slot_.badge_text.c_str());
        gtk_widget_set_visible(badge_, TRUE);
    }
}

void StatusWidget::set_status(const Slot& new_slot) {
    slot_ = new_slot;
    gtk_widget_remove_css_class(button_, "realmheart-bar-status-enabled");
    gtk_widget_remove_css_class(button_, "realmheart-bar-status-disabled");
    gtk_widget_add_css_class(
        button_,
        slot_.enabled
            ? "realmheart-bar-status-enabled"
            : "realmheart-bar-status-disabled"
    );
    gtk_widget_set_tooltip_text(button_, slot_.tooltip.c_str());
    update_badge();
}

} // namespace realmheart::ui::components
