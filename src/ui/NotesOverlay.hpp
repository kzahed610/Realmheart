#pragma once

#include <gtk/gtk.h>
#include "services/NotesService.hpp"

namespace realmheart::ui {

class NotesOverlay {
public:
    NotesOverlay(GtkApplication* app, services::NotesService* notes_service);
    ~NotesOverlay();

    void show();
    void hide();
    void toggle();

private:
    GtkWidget* window_ = nullptr;
    GtkWidget* text_view_ = nullptr;
    GtkTextBuffer* buffer_ = nullptr;
    services::NotesService* notes_service_ = nullptr;

    static void on_text_changed_callback(GtkTextBuffer* buf, gpointer data);
};

} // namespace realmheart::ui
