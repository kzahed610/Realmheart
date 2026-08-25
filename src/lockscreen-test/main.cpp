// Standalone harness for the lockscreen surface.
// Renders the input bar + scales on the live Wayland session without PAM or
// input grab. Press Esc or close the window to exit.
// --auto-unlock N: after N seconds, fire the unlocked callback and hide the
// surface (exercises the closing/erosion path without real auth).

#include "ui/lockscreen/LockSurface.hpp"

#include "ui/styles/CssModuleLoader.hpp"

#include <gtk/gtk.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

constexpr int kTimeoutSeconds = 30;

struct TestState {
    GtkApplication* application = nullptr;
    std::unique_ptr<realmheart::ui::lockscreen::LockSurface> surface;
    gboolean activate_fired = FALSE;
    int auto_unlock_after_seconds = 0;
    bool unlocked = false;
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
    std::cout << "[lockscreen test] activate fired" << std::endl;

    // Construct the surface inside activate so the GTK display exists before
    // the surface is presented.
    state->surface = std::make_unique<realmheart::ui::lockscreen::LockSurface>(app);

    // Same modular CSS the shell loads, so title/entry styling matches.
    {
        static const std::array<std::string_view, 1> kModules{
            "lockscreen/lockscreen.css"
        };
        const std::string css = realmheart::ui::styles::load_css_modules(kModules);
        auto* provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, css.c_str());
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
        );
        g_object_unref(provider);
    }

    state->surface->set_unlocked_callback([state] {
        std::cout << "[lockscreen test] unlocked callback fired\n";
        state->unlocked = true;
    });
    state->surface->show();

    // Optional auto-unlock: exercise the closing path without real auth.
    if (state->auto_unlock_after_seconds > 0) {
        g_timeout_add_seconds(
            state->auto_unlock_after_seconds,
            +[](gpointer data) -> gboolean {
                auto* state = static_cast<TestState*>(data);
                if (!state->unlocked) {
                    std::cout << "[lockscreen test] auto-unlock: hiding surface\n";
                    state->surface->hide();
                }
                return G_SOURCE_REMOVE;
            },
            state
        );
    }

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

    std::cout << "[lockscreen test] surface presented (namespace "
                 "realmheart-broken_seal)\n";

    // Debug: report focus state shortly after present.
    g_timeout_add(3000, +[](gpointer data) -> gboolean {
        auto* surface = static_cast<realmheart::ui::lockscreen::LockSurface*>(data);
        GtkWidget* window = GTK_WIDGET(surface->window());
        GtkWidget* focused = gtk_window_get_focus(GTK_WINDOW(window));
        std::cout << "[lockscreen test] focus: "
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
    // --auto-unlock N: fire the unlock path after N seconds. Strip it from
    // argv before GTK parses the rest.
    int out_argc = 0;
    char** out_argv = new char*[argc];
    for (int i = 0; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--auto-unlock") {
            if (i + 1 < argc) {
                state->auto_unlock_after_seconds = std::atoi(argv[i + 1]);
                ++i; // consume the value
            }
            continue;
        }
        out_argv[out_argc++] = argv[i];
    }
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

    const int status = g_application_run(G_APPLICATION(application), out_argc, out_argv);
    delete[] out_argv;
    state.release();
    g_object_unref(application);
    return status;
}
