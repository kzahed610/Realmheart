#include "ui/components/StatusWidget.hpp"
#include "ui/AssetResolver.hpp"
#include <gtk/gtk.h>

namespace realmheart::ui::components {

StatusWidget::StatusWidget(const Slot& slot, std::function<void()> on_click) 
    : slot_(slot), on_click_(on_click) {
    
    box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(box_, 12);
    gtk_widget_set_margin_end(box_, 12);
    gtk_widget_set_margin_top(box_, 6);
    gtk_widget_set_margin_bottom(box_, 6);

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

    if (!slot_.badge_text.empty()) {
        badge_ = gtk_label_new(slot_.badge_text.c_str());
        gtk_widget_add_css_class(badge_, "realmheart-bar-badge");
        gtk_widget_set_halign(badge_, GTK_ALIGN_END);
        gtk_widget_set_valign(badge_, GTK_ALIGN_START);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), badge_);
    }

    gtk_box_append(GTK_BOX(box_), overlay_);
    
    auto* callback = new std::function<void()>(on_click_);
    g_signal_connect_data(box_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        (*static_cast<std::function<void()>*>(data))();
    }), callback, +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, G_CONNECT_DEFAULT);
}

GtkWidget* StatusWidget::get_widget() {
    return box_;
}

void StatusWidget::refresh() {
    // Handled by set_status usually, but we could trigger a probe here
}

void StatusWidget::set_status(const Slot& new_slot) {
    slot_ = new_slot;
    gtk_widget_add_css_class(box_, slot_.enabled ? "realmheart-bar-status-enabled" : "realmheart-bar-status-disabled");
    gtk_widget_remove_css_class(box_, slot_.enabled ? "realmheart-bar-status-disabled" : "realmheart-bar-status-enabled");
    gtk_widget_set_tooltip_text(box_, slot_.tooltip.c_str());
    
    if (badge_) {
        if (!slot_.badge_text.empty()) {
            gtk_label_set_text(GTK_LABEL(badge_), slot_.badge_text.c_str());
        } else {
            gtk_widget_set_visible(badge_, FALSE);
        }
    }
}

void StatusWidget::apply_theme(const services::Palette& palette) {
    std::string surface_color = palette.get("surface", "#1e1e2e");
    std::string accent_color = palette.get("accent", "#cba6f7");
    std::string text_color = palette.get("text", "#cdd6f4");

    std::string css = ".status-widget { background: " + surface_color + "; color: " + text_color + "; }\n"
                      ".status-widget-enabled { border-color: " + accent_color + "; }\n"
                      ".status-widget-badge { background: " + palette.get("red", "#f38ba8") + "; color: #11111b; }";
    
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css.c_str());
    
    gtk_style_context_add_provider(gtk_widget_get_style_context(box_), 
                                   GTK_STYLE_PROVIDER(provider), 
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    
    gtk_widget_add_css_class(box_, "status-widget");
    if (badge_) gtk_widget_add_css_class(badge_, "status-widget-badge");
    
    g_object_unref(provider);
}

} // namespace realmheart::ui::components
