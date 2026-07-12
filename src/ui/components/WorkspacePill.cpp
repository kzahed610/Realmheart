#include "ui/components/WorkspacePill.hpp"

#include "core/Command.hpp"

namespace realmheart::ui::components {

WorkspacePill::WorkspacePill(const WorkspaceState& state)
    : workspace_id_(state.id) {
    button_ = gtk_button_new_with_label(std::to_string(state.id).c_str());
    gtk_widget_add_css_class(button_, "realmheart-bar-pill");
    gtk_widget_set_halign(button_, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(button_, 4);
    gtk_widget_set_margin_bottom(button_, 4);

    g_object_set_data(
        G_OBJECT(button_),
        "realmheart-workspace-id",
        GINT_TO_POINTER(state.id)
    );
    g_signal_connect(button_, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer) {
        const int id = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(button), "realmheart-workspace-id")
        );
        realmheart::core::run_background({
            "hyprctl",
            "dispatch",
            "workspace",
            std::to_string(id)
        });
    }), nullptr);

    update(state);
}

GtkWidget* WorkspacePill::get_widget() {
    return button_;
}

void WorkspacePill::update(const WorkspaceState& state) {
    workspace_id_ = state.id;
    gtk_button_set_label(GTK_BUTTON(button_), std::to_string(state.id).c_str());
    g_object_set_data(
        G_OBJECT(button_),
        "realmheart-workspace-id",
        GINT_TO_POINTER(state.id)
    );

    gtk_widget_remove_css_class(button_, "realmheart-bar-pill-active");
    gtk_widget_remove_css_class(button_, "realmheart-bar-pill-occupied");
    if (state.active) gtk_widget_add_css_class(button_, "realmheart-bar-pill-active");
    if (state.windows > 0) gtk_widget_add_css_class(button_, "realmheart-bar-pill-occupied");

    std::string tooltip = "Workspace " + std::to_string(state.id);
    if (!state.name.empty() && state.name != std::to_string(state.id)) {
        tooltip += " (" + state.name + ")";
    }
    tooltip += state.active ? ": active" : ": inactive";
    tooltip += ", windows=" + std::to_string(state.windows);
    gtk_widget_set_tooltip_text(button_, tooltip.c_str());
}

} // namespace realmheart::ui::components
