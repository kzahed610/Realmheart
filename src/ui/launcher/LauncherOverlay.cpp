#include "ui/launcher/LauncherOverlay.hpp"

#include "ui/LayerSurface.hpp"

#include <gtk4-layer-shell.h>

#include <algorithm>

namespace realmheart::ui {

LauncherOverlay::LauncherOverlay(GtkApplication* app, services::LauncherService& service)
    : service_(service) {
    window_ = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_decorated(window_, FALSE);

    setup_window();
    setup_ui();
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

LauncherOverlay::~LauncherOverlay() {
    if (window_ != nullptr) {
        gtk_window_destroy(window_);
    }
}

void LauncherOverlay::setup_window() {
    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-launcher";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.anchor_top = true;
    spec.anchor_left = true;
    spec.anchor_right = true;
    apply_layer_surface(window_, spec);

    gtk_layer_set_keyboard_mode(window_, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_window_set_default_size(window_, 600, 400);
}

void LauncherOverlay::setup_ui() {
    GtkWidget* main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(main_box, 40);
    gtk_widget_set_margin_end(main_box, 40);
    gtk_widget_set_margin_top(main_box, 40);
    gtk_widget_set_margin_bottom(main_box, 40);

    search_entry_ = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry_), "Search applications or enter a command…");
    g_signal_connect(search_entry_, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) {
        static_cast<LauncherOverlay*>(data)->on_search_changed();
    }), this);
    g_signal_connect(search_entry_, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data) {
        static_cast<LauncherOverlay*>(data)->activate_result(0);
    }), this);

    results_list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(results_list_), GTK_SELECTION_SINGLE);
    g_signal_connect(results_list_, "row-activated", G_CALLBACK(+[](
        GtkListBox*, GtkListBoxRow* row, gpointer data
    ) {
        const int index = gtk_list_box_row_get_index(row);
        if (index >= 0) {
            static_cast<LauncherOverlay*>(data)->activate_result(static_cast<std::size_t>(index));
        }
    }), this);

    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), results_list_);

    gtk_box_append(GTK_BOX(main_box), search_entry_);
    gtk_box_append(GTK_BOX(main_box), scrolled);
    gtk_window_set_child(window_, main_box);

    GtkEventController* key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(+[](
        GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data
    ) -> gboolean {
        if (keyval == GDK_KEY_Escape) {
            static_cast<LauncherOverlay*>(data)->hide();
            return TRUE;
        }
        return FALSE;
    }), this);
    gtk_widget_add_controller(GTK_WIDGET(window_), key_controller);
}

void LauncherOverlay::on_search_changed() {
    const char* text = gtk_editable_get_text(GTK_EDITABLE(search_entry_));
    current_results_ = service_.search(text != nullptr ? text : "", 10);

    GtkWidget* child = gtk_widget_get_first_child(results_list_);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(results_list_), child);
        child = next;
    }

    for (const auto& result : current_results_) {
        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget* icon = gtk_image_new_from_icon_name(
            result.icon_name.empty() ? "application-x-executable" : result.icon_name.c_str()
        );
        GtkWidget* label = gtk_label_new(result.title.c_str());
        gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
        gtk_widget_set_hexpand(label, TRUE);

        gtk_box_append(GTK_BOX(box), icon);
        gtk_box_append(GTK_BOX(box), label);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(GTK_LIST_BOX(results_list_), row);
    }

    if (!current_results_.empty()) {
        GtkListBoxRow* first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(results_list_), 0);
        gtk_list_box_select_row(GTK_LIST_BOX(results_list_), first);
    }
}

void LauncherOverlay::activate_result(std::size_t index) {
    if (index >= current_results_.size()) {
        return;
    }

    if (service_.activate(current_results_[index])) {
        hide();
    }
}

void LauncherOverlay::toggle() {
    if (gtk_widget_get_visible(GTK_WIDGET(window_))) {
        hide();
    } else {
        show();
    }
}

void LauncherOverlay::show() {
    gtk_editable_set_text(GTK_EDITABLE(search_entry_), "");
    on_search_changed();
    gtk_window_present(window_);
    gtk_widget_grab_focus(search_entry_);
}

void LauncherOverlay::hide() {
    gtk_widget_set_visible(GTK_WIDGET(window_), FALSE);
}

} // namespace realmheart::ui
