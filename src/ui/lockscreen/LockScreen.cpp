#include "ui/lockscreen/LockScreen.hpp"
#include "ui/LayerSurface.hpp"
#include <gtk/gtk.h>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace realmheart::ui {

LockScreen::LockScreen(GtkApplication* app) : app_(app) {
    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Lock Screen");
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    
    // Exclusive layer surface to block input to everything else
    apply_layer_shell();
    setup_layout();
}

void LockScreen::apply_layer_shell() {
    apply_layer_surface(GTK_WINDOW(window_), make_layer_surface_spec(
        "realmheart-lockscreen", 
        LayerSurfaceLevel::Overlay, 
        LayerKeyboardMode::Exclusive
    ));
}

void LockScreen::setup_layout() {
    // Main container: Full screen overlay
    overlay_container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(overlay_container_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(overlay_container_, GTK_ALIGN_CENTER);
    
    // Visuals: centered-clock Realmheart lock screen presentation
    clock_label_ = gtk_label_new("00:00");
    
    // Use Pango markup for that "Aesthetic as fuck" look
    const char* clock_style = "<span font='48' weight='bold' foreground='#cdd6f4'>00:00</span>";
    gtk_label_set_markup(GTK_LABEL(clock_label_), clock_style);
    
    gtk_box_append(GTK_BOX(overlay_container_), clock_label_);
    
    // Input field: Simple password entry (MVP)
    GtkWidget* entry = gtk_password_entry_new();
    gtk_widget_set_margin_top(entry, 20);
    gtk_widget_set_halign(entry, GTK_ALIGN_CENTER);
    
    gtk_box_append(GTK_BOX(overlay_container_), entry);
    
    gtk_window_set_child(GTK_WINDOW(window_), overlay_container_);
}

// Destructor handled by = default in header


} // namespace realmheart::ui
