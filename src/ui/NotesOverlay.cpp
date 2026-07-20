#include "ui/NotesOverlay.hpp"
#include "ui/LayerSurface.hpp"
#include <gtk/gtk.h>
#include <iostream>

namespace realmheart::ui {

NotesOverlay::NotesOverlay(GtkApplication* app, services::NotesService* notes_service) 
    : notes_service_(notes_service) {
    lifetime_->owner = this;
    
    window_ = GTK_WIDGET(gtk_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Notes");
    gtk_window_set_default_size(GTK_WINDOW(window_), 600, 800);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    
    LayerSurfaceSpec spec;
    spec.surface_namespace = "realmheart-notes";
    spec.layer = LayerSurfaceLevel::Overlay;
    spec.keyboard_mode = LayerKeyboardMode::Exclusive;
    apply_layer_surface(GTK_WINDOW(window_), spec);

    gtk_widget_add_css_class(window_, "realmheart-notes");

    buffer_ = gtk_text_buffer_new(nullptr);
    text_view_ = gtk_text_view_new_with_buffer(buffer_);
    gtk_widget_add_css_class(text_view_, "realmheart-notes-editor");
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view_), GTK_WRAP_WORD);
    
    gtk_text_buffer_set_text(buffer_, notes_service_->get_content().c_str(), -1);

    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), text_view_);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(root), scrolled);
    status_label_ = gtk_label_new("Saved");
    gtk_widget_add_css_class(status_label_, "realmheart-notes-save-state");
    gtk_label_set_xalign(GTK_LABEL(status_label_), 1.0F);
    gtk_box_append(GTK_BOX(root), status_label_);
    gtk_window_set_child(GTK_WINDOW(window_), root);

    g_signal_connect(buffer_, "changed", G_CALLBACK(on_text_changed_callback), this);

    const auto lifetime = lifetime_;
    notes_service_->set_save_state_callback([lifetime](services::NotesSaveState state) {
        struct Payload {
            std::shared_ptr<LifetimeState> lifetime;
            services::NotesSaveState state;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                if (payload->lifetime->alive.load() &&
                    payload->lifetime->owner != nullptr) {
                    payload->lifetime->owner->apply_save_state(payload->state);
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{lifetime, state},
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    });

    gtk_widget_set_visible(window_, FALSE);
}

NotesOverlay::~NotesOverlay() {
    notes_service_->set_save_state_callback({});
    lifetime_->alive = false;
    lifetime_->owner = nullptr;
    if (buffer_ != nullptr) {
        g_signal_handlers_disconnect_by_data(buffer_, this);
    }
    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void NotesOverlay::apply_save_state(services::NotesSaveState state) {
    if (status_label_ == nullptr) return;
    gtk_widget_remove_css_class(status_label_, "pending");
    gtk_widget_remove_css_class(status_label_, "failed");
    switch (state) {
    case services::NotesSaveState::Saved:
        gtk_label_set_text(GTK_LABEL(status_label_), "Saved");
        break;
    case services::NotesSaveState::Pending:
        gtk_label_set_text(GTK_LABEL(status_label_), "Saving…");
        gtk_widget_add_css_class(status_label_, "pending");
        break;
    case services::NotesSaveState::Failed:
        gtk_label_set_text(GTK_LABEL(status_label_), "Save failed");
        gtk_widget_add_css_class(status_label_, "failed");
        break;
    }
}

void NotesOverlay::on_text_changed_callback(GtkTextBuffer* buf, gpointer data) {
    auto* self = static_cast<NotesOverlay*>(data);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    char* text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    self->notes_service_->set_content(text);
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
