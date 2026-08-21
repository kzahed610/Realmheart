// Standalone harness for the Broken Seal lockscreen surface.
// Renders the horn pair on the live Wayland session without PAM or input
// grab. Press Esc or close the window to exit.

#include "services/SessionManager.hpp"
#include "ui/lockscreen/LockSurface.hpp"

#include <gtk/gtk.h>

#include <iostream>
#include <memory>

namespace {

constexpr int kTimeoutSeconds = 15;

struct TestState {
    GtkApplication* application = nullptr;
    std::unique_ptr<realmheart::ui::lockscreen::LockSurface> surface;
    std::unique_ptr<realmheart::services::SessionManager> session;
    gboolean activate_fired = FALSE;
};

gboolean on_escape(
    GtkEventControllerKey*,
    guint keyval,
    guint,
    GdkModifierType,
    gpointer data
) {
    if (keyval != GDK_KEY_Escape) return GDK_EVENT_PROPAGATE;
    g_application_quit(G_APPLICATION(data));
    return GDK_EVENT_STOP;
}

void on_activate(GtkApplication* app, gpointer data) {
    auto* state = static_cast<TestState*>(data);
    state->application = app;
    state->activate_fired = TRUE;
    std::cout << "[BrokenSeal test] activate fired" << std::endl;

    // Construct the surface inside activate so the GTK display exists before
    // the GtkGLArea widget is created.
    state->surface = std::make_unique<realmheart::ui::lockscreen::LockSurface>(app);

    // Apply the Hyprland blur layerrule BEFORE the surface maps so the rule
    // matches at map time (Hyprland layerrules only match new surfaces).
    state->session = std::make_unique<realmheart::services::SessionManager>();
    state->session->enable_lockscreen_blur();

    state->surface->show();

    GtkEventController* key_controller = gtk_event_controller_key_new();
    g_signal_connect(
        key_controller,
        "key-pressed",
        G_CALLBACK(on_escape),
        app
    );
    gtk_widget_add_controller(
        GTK_WIDGET(state->surface->window()),
        key_controller
    );

    std::cout << "[BrokenSeal test] surface presented (namespace "
                 "realmheart-broken_seal)\n";

    // Debug: report focus state after the animation completes.
    g_timeout_add(3000, +[](gpointer data) -> gboolean {
        auto* surface = static_cast<realmheart::ui::lockscreen::LockSurface*>(data);
        GtkWidget* window = GTK_WIDGET(surface->window());
        GtkWidget* focused = gtk_window_get_focus(GTK_WINDOW(window));
        std::cout << "[BrokenSeal test] focus: "
                  << (focused != nullptr ? gtk_widget_get_name(focused) : "(none)")
                  << " mapped=" << gtk_widget_get_mapped(window)
                  << " visible=" << gtk_widget_get_visible(window)
                  << std::endl;
        return G_SOURCE_REMOVE;
    }, state->surface.get());
}

} // namespace

int main(int argc, char** argv) {
    GtkApplication* application = gtk_application_new(
        "dev.realmheart.lockscreen-test",
        G_APPLICATION_NON_UNIQUE
    );

    auto state = std::make_unique<TestState>();
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), state.get());

    // Fallback quit after kTimeoutSeconds so a stalled session can't hang.
    g_timeout_add_seconds(
        kTimeoutSeconds,
        +[](gpointer data) -> gboolean {
            g_application_quit(G_APPLICATION(data));
            return G_SOURCE_REMOVE;
        },
        application
    );

    const int status = g_application_run(G_APPLICATION(application), argc, argv);
    // Intentionally leak state so the LockSurface destructor never runs after
    // the GL context is gone (g_application_run tears it down on return).
    // The process exit reclaims the surface and its GL programs, matching the
    // power-menu renderer's approach.
    state.release();
    g_object_unref(application);
    return status;
}
