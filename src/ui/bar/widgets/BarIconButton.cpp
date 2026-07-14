#include "ui/bar/widgets/BarIconButton.hpp"

#include "ui/AssetResolver.hpp"

#include <algorithm>
#include <utility>

namespace realmheart::ui::bar::widgets {

BarIconButton::BarIconButton(
    std::string asset_path,
    std::string fallback_text,
    std::string tooltip,
    std::function<void()> on_click
) : on_click_(std::move(on_click)) {
    button_ = gtk_button_new();
    gtk_widget_add_css_class(button_, "realmheart-bar-icon-button");
    gtk_widget_set_halign(button_, GTK_ALIGN_CENTER);

    overlay_ = gtk_overlay_new();
    stack_ = gtk_stack_new();
    icon_ = gtk_image_new();
    gtk_widget_add_css_class(icon_, "realmheart-bar-svg-icon");
    gtk_widget_set_halign(icon_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(icon_, GTK_ALIGN_CENTER);
    set_icon_size(31);

    fallback_ = gtk_label_new(nullptr);
    gtk_widget_add_css_class(fallback_, "realmheart-bar-fallback-icon");
    gtk_stack_add_named(GTK_STACK(stack_), icon_, "image");
    gtk_stack_add_named(GTK_STACK(stack_), fallback_, "fallback");
    gtk_overlay_set_child(GTK_OVERLAY(overlay_), stack_);
    gtk_button_set_child(GTK_BUTTON(button_), overlay_);

    if (on_click_) {
        click_handler_ = g_signal_connect(
            button_,
            "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                auto* self = static_cast<BarIconButton*>(data);
                if (self->on_click_) self->on_click_();
            }),
            this
        );
    }

    set_icon(std::move(asset_path), std::move(fallback_text));
    set_tooltip(tooltip);
}

BarIconButton::~BarIconButton() {
    if (button_ != nullptr && click_handler_ != 0) {
        g_signal_handler_disconnect(button_, click_handler_);
    }
}

void BarIconButton::set_icon(std::string asset_path, std::string fallback_text) {
    if (const auto resolved = resolve_project_asset(asset_path)) {
        gtk_image_set_from_file(GTK_IMAGE(icon_), resolved->string().c_str());
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "image");
    } else {
        gtk_image_clear(GTK_IMAGE(icon_));
        gtk_label_set_text(GTK_LABEL(fallback_), fallback_text.c_str());
        gtk_stack_set_visible_child_name(GTK_STACK(stack_), "fallback");
    }
}

void BarIconButton::set_tooltip(const std::string& tooltip) {
    gtk_widget_set_tooltip_text(button_, tooltip.c_str());
}

void BarIconButton::set_enabled(bool enabled) {
    gtk_widget_remove_css_class(button_, "realmheart-bar-icon-enabled");
    gtk_widget_remove_css_class(button_, "realmheart-bar-icon-disabled");
    gtk_widget_add_css_class(
        button_,
        enabled ? "realmheart-bar-icon-enabled" : "realmheart-bar-icon-disabled"
    );
}

void BarIconButton::set_badge(const std::string& text) {
    if (text.empty()) {
        if (badge_ != nullptr) gtk_widget_set_visible(badge_, FALSE);
        return;
    }
    if (badge_ == nullptr) {
        badge_ = gtk_label_new(text.c_str());
        gtk_widget_add_css_class(badge_, "realmheart-bar-badge");
        gtk_widget_set_halign(badge_, GTK_ALIGN_END);
        gtk_widget_set_valign(badge_, GTK_ALIGN_START);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), badge_);
    } else {
        gtk_label_set_text(GTK_LABEL(badge_), text.c_str());
        gtk_widget_set_visible(badge_, TRUE);
    }
}

void BarIconButton::add_css_class(const char* css_class) {
    if (css_class != nullptr && *css_class != '\0') gtk_widget_add_css_class(button_, css_class);
}

void BarIconButton::set_icon_size(int pixels) {
    const int size = std::clamp(pixels, 14, 38);
    gtk_image_set_pixel_size(GTK_IMAGE(icon_), size);
}

} // namespace realmheart::ui::bar::widgets
