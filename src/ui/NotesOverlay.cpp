#include "ui/NotesOverlay.hpp"
#include "ui/LayerSurface.hpp"
#include <gtk/gtk.h>
#include <iostream>

namespace realmheart::ui {

NotesOverlay::NotesOverlay(GtkApplication* app, services::NotesService* notes_service) 
    : notes_service_(notes_service) {
    
    window_ = GTK_WIDGET(gtk_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Notes");
    gtk_window_set_default_size(GTK_WINDOW(window_), 600, 800);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    
    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-notes";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.keyboard_mode = LayerKeyboardMode::Exclusive;
    apply_layer_surface(GTK_WINDOW(window_), spec);

    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, 
        "window { background-color: rgba(30, 30, 46, 0.9); color: #cdd6f4; border: 2px solid #89b4fa; border-radius: 12px; } "
        "textview { background-color: transparent; color: #cdd6f4; font-family: 'JetBrains Mono'; font-size: 14px; }");
    
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );

    buffer_ = gtk_text_buffer_new(nullptr);
    text_view_ = gtk_text_view_new_with_buffer(buffer_);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view_), GTK_WRAP_WORD);
    
    gtk_text_buffer_set_text(buffer_, notes_service_->get_content().c_str(), -1);

    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), text_view_);
    gtk_window_set_child(GTK_WINDOW(window_), scrolled);

    g_signal_connect(buffer_, "changed", G_CALLBACK(on_text_changed_callback), this);

    gtk_widget_set_visible(window_, FALSE);
}

void NotesOverlay::on_text_changed_callback(GtkTextBuffer* buf, gpointer data) {
    auto* self = static_cast<NotesOverlay*>(data);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    char* text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    self->notes_service_->set_content(text);
    self->notes_service_->save();
    g_free(text);
}

void NotesOverlay::show() {
    gtk_widget_set_visible(window_, TRUE);
    gtk_window_present(GTK_WINDOW(window_));
}

void NotesOverlay::hide() {
    gtk_widget_set_visible(window_, FALSE);
}

void NotesOverlay::toggle() {
    gtk_widget_set_visible(window_, !gtk_widget_get_visible(window_));
    if (gtk_widget_get_visible(window_)) {
        gtk_window_present(GTK_WINDOW(window_));
    }
}

} // namespace realmheart::ui
