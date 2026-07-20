#pragma once

#include <gtk/gtk.h>
#include "services/NotesService.hpp"

#include <atomic>
#include <memory>

namespace realmheart::ui {

class NotesOverlay {
public:
    NotesOverlay(GtkApplication* app, services::NotesService* notes_service);
    ~NotesOverlay();

    void show();
    void hide();
    void toggle();

private:
    struct LifetimeState {
        std::atomic<bool> alive{true};
        NotesOverlay* owner = nullptr; // GTK main thread only
    };

    GtkWidget* window_ = nullptr;
    GtkWidget* text_view_ = nullptr;
    GtkWidget* status_label_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    services::NotesService* notes_service_ = nullptr;
    std::shared_ptr<LifetimeState> lifetime_ = std::make_shared<LifetimeState>();

    static void on_text_changed_callback(GtkTextBuffer* buf, gpointer data);
    void apply_save_state(services::NotesSaveState state);
};

} // namespace realmheart::ui
