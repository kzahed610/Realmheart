#include "ui/components/WorkspacePill.hpp"
#include <gtk/gtk.h>
#include "core/Command.hpp"

namespace realmheart::ui::components {

WorkspacePill::WorkspacePill(const WorkspaceState& state) {
    label_ = gtk_button_new_with_label(std::to_string(state.id).c_str());
    gtk_widget_add_css_class(label_, "realmheart-bar-pill");
    if (state.active) gtk_widget_add_css_class(label_, "realmheart-bar-pill-active");
    if (state.windows > 0) gtk_widget_add_css_class(label_, "realmheart-bar-pill-occupied");
    gtk_widget_set_halign(label_, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(label_, 4);
    gtk_widget_set_margin_bottom(label_, 4);

    std::string tooltip = "Workspace " + std::to_string(state.id);
    if (!state.name.empty() && state.name != std::to_string(state.id)) {
        tooltip += " (" + state.name + ")";
    }
    tooltip += state.active ? ": active" : ": inactive";
    tooltip += ", windows=" + std::to_string(state.windows);
    gtk_widget_set_tooltip_text(label_, tooltip.c_str());

    g_object_set_data(G_OBJECT(label_), "realmheart-workspace-id", GINT_TO_POINTER(state.id));
    g_signal_connect(label_, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer) {
        const int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "realmheart-workspace-id"));
        realmheart::core::run_background({
            "hyprctl",
            "dispatch",
            "hl.dsp.focus({ workspace = " + std::to_string(id) + " })"
        });
    }), nullptr);

    // Initialize provider once
    provider_ = gtk_css_provider_new();
    gtk_style_context_add_provider(gtk_widget_get_style_context(label_), 
                                   GTK_STYLE_PROVIDER(provider_), 
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

GtkWidget* WorkspacePill::get_widget() {
    return label_;
}

void WorkspacePill::update(const WorkspaceState& state) {
    gtk_label_set_text(GTK_LABEL(label_), std::to_string(state.id).c_str());
    
    gtk_widget_remove_css_class(label_, "realmheart-bar-pill-active");
    if (state.active) gtk_widget_add_css_class(label_, "realmheart-bar-pill-active");
    
    gtk_widget_remove_css_class(label_, "realmheart-bar-pill-occupied");
    if (state.windows > 0) gtk_widget_add_css_class(label_, "realmheart-bar-pill-occupied");
    
    std::string tooltip = "Workspace " + std::to_string(state.id);
    if (!state.name.empty() && state.name != std::to_string(state.id)) {
        tooltip += " (" + state.name + ")";
    }
    tooltip += state.active ? ": active" : ": inactive";
    tooltip += ", windows=" + std::to_string(state.windows);
    gtk_widget_set_tooltip_text(label_, tooltip.c_str());
}

void WorkspacePill::apply_theme(const services::Palette& palette) {
    std::string bg_color = palette.get("surface", "#313244");
    std::string accent_color = palette.get("accent", "#cba6f7");
    std::string text_color = palette.get("text", "#bac2de");

    std::string css = ".realmheart-bar-pill { background: " + bg_color + "; color: " + text_color + "; }\n"
                      ".realmheart-bar-pill-active { background: " + accent_color + "44; border-color: " + accent_color + "; }\n"
                      ".realmheart-bar-pill-occupied { color: " + palette.get("blue", "#89b4fa") + "; }";
    
    gtk_css_provider_load_from_string(provider_, css.c_str());
}

} // namespace realmheart::ui::components
