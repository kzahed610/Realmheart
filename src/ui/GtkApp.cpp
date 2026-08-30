#include "ui/GtkApp.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/lockscreen/LockSurface.hpp"
#include "ui/bar/VerticalBar.hpp"
#include "ui/sidebar/RightSidebar.hpp"
#include "ui/ThemeStyles.hpp"
#include "services/BatteryService.hpp"
#include "services/MediaService.hpp"
#include "services/ThemeService.hpp"
#include "services/Notifications.hpp"

#include <gtk/gtk.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace realmheart::ui {

// Internal state for timed application runs
struct TimedLayerState {
    GtkApplication* application = nullptr;
    int timeout_seconds = 5;
    guint quit_timer_id = 0;
};

// Helper to quit the application after a timeout
gboolean quit_application_callback(gpointer user_data) {
    auto* state = static_cast<TimedLayerState*>(user_data);
    state->quit_timer_id = 0;
    g_application_quit(G_APPLICATION(state->application));
    return G_SOURCE_REMOVE;
}

void schedule_application_quit(TimedLayerState* state) {
    constexpr int kMin = 1;
    constexpr int kMax = 3600;
    const auto timeout = std::clamp(state->timeout_seconds, kMin, kMax);
    state->quit_timer_id = g_timeout_add_seconds(static_cast<guint>(timeout), quit_application_callback, state);
}

// Escape key handler
gboolean handle_escape(GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer user_data) {
    if (keyval != GDK_KEY_Escape) return GDK_EVENT_PROPAGATE;
    g_application_quit(G_APPLICATION(user_data));
    return GDK_EVENT_STOP;
}

void attach_escape_controller(GtkWidget* window, GtkApplication* application) {
    GtkEventController* key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(handle_escape), application);
    gtk_widget_add_controller(window, key_controller);
}

void add_css_provider(std::string_view css) {
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, std::string(css).c_str());
    if (GdkDisplay* display = gdk_display_get_default(); display != nullptr) {
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

void install_test_layer_css() {
    add_css_provider(
        ".realmheart-test-layer {"
        "  background: alpha(#1e1e2e, 0.88);"
        "  border: 1px solid alpha(#cba6f7, 0.75);"
        "  border-radius: 14px;"
        "  box-shadow: 0 12px 32px alpha(#000000, 0.38);"
        "}"
        ".realmheart-test-layer label {"
        "  color: #cdd6f4; font-weight: 700; letter-spacing: 0.02em;"
        "}"
    );
}

// --- Activation Callbacks ---

void activate_test_layer(GtkApplication* app, gpointer data) {
    auto* state = static_cast<TimedLayerState*>(data);
    schedule_application_quit(state);
    install_test_layer_css();

    GtkWidget* window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Realmheart test layer");
    gtk_window_set_default_size(GTK_WINDOW(window), 260, 64);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    apply_layer_surface(GTK_WINDOW(window), make_test_surface_spec());

    GtkWidget* frame = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(frame, "realmheart-test-layer");
    gtk_widget_set_size_request(frame, 260, 64);

    GtkWidget* label = gtk_label_new("Realmheart test layer");
    gtk_widget_set_margin_start(label, 18);
    gtk_widget_set_margin_end(label, 18);
    gtk_widget_set_margin_top(label, 14);
    gtk_widget_set_margin_bottom(label, 14);
    gtk_box_append(GTK_BOX(frame), label);
    attach_escape_controller(window, app);
    gtk_window_set_child(GTK_WINDOW(window), frame);
    gtk_window_present(GTK_WINDOW(window));
}

struct TimedBarController {
    services::NotificationHistory notification_history;
    services::BatteryService battery;
    services::MediaService media;
    std::shared_ptr<services::ThemeService> theme_service =
        std::make_shared<services::ThemeService>();
    std::unique_ptr<ThemeStyles> theme_styles;
    std::unique_ptr<bar::VerticalBar> bar;
};

struct TimedSidebarController {
    services::NotificationHistory notification_history;
    std::shared_ptr<services::ThemeService> theme_service =
        std::make_shared<services::ThemeService>();
    std::unique_ptr<ThemeStyles> theme_styles;
    std::unique_ptr<sidebar::RightSidebar> sidebar;
};

void activate_lockscreen_test(GtkApplication* app, gpointer data) {
    auto* state = static_cast<TimedLayerState*>(data);
    schedule_application_quit(state);

    auto* surface = static_cast<lockscreen::LockSurface*>(
        g_object_get_data(G_OBJECT(app), "realmheart-timed-lockscreen-surface")
    );
    if (surface == nullptr) {
        auto owned = std::make_unique<lockscreen::LockSurface>(app);
        surface = owned.release();
        g_object_set_data_full(
            G_OBJECT(app),
            "realmheart-timed-lockscreen-surface",
            surface,
            +[](gpointer value) { delete static_cast<lockscreen::LockSurface*>(value); }
        );
    }

    attach_escape_controller(GTK_WIDGET(surface->window()), app);
    surface->show();
}

void activate_bar(GtkApplication* app, gpointer data) {
    auto* state = static_cast<TimedLayerState*>(data);
    schedule_application_quit(state);

    auto* controller = static_cast<TimedBarController*>(
        g_object_get_data(G_OBJECT(app), "realmheart-timed-bar-controller")
    );
    if (controller == nullptr) {
        auto owned = std::make_unique<TimedBarController>();
        owned->theme_styles = std::make_unique<ThemeStyles>(owned->theme_service);
        owned->bar = std::make_unique<bar::VerticalBar>(
            app,
            owned->notification_history,
            owned->battery,
            owned->media,
            [] {},
            [] {}
        );
        controller = owned.release();
        g_object_set_data_full(
            G_OBJECT(app),
            "realmheart-timed-bar-controller",
            controller,
            +[](gpointer value) { delete static_cast<TimedBarController*>(value); }
        );
    }

    attach_escape_controller(controller->bar->get_window(), app);
    gtk_window_present(GTK_WINDOW(controller->bar->get_window()));
}

void activate_sidebar(GtkApplication* app, gpointer data) {
    auto* state = static_cast<TimedLayerState*>(data);
    schedule_application_quit(state);

    auto* controller = static_cast<TimedSidebarController*>(
        g_object_get_data(G_OBJECT(app), "realmheart-timed-sidebar-controller")
    );
    if (controller == nullptr) {
        auto owned = std::make_unique<TimedSidebarController>();
        owned->theme_styles = std::make_unique<ThemeStyles>(owned->theme_service);
        owned->sidebar = std::make_unique<sidebar::RightSidebar>(
            app,
            owned->notification_history
        );
        controller = owned.release();
        g_object_set_data_full(
            G_OBJECT(app),
            "realmheart-timed-sidebar-controller",
            controller,
            +[](gpointer value) { delete static_cast<TimedSidebarController*>(value); }
        );
    }

    attach_escape_controller(controller->sidebar->get_window(), app);
    controller->sidebar->refresh();
    controller->sidebar->apply_geometry();
    gtk_window_present(GTK_WINDOW(controller->sidebar->get_window()));
}

// --- Core Runner ---

int run_timed_application(const char* application_id, int timeout_seconds, GCallback activate_callback) {
    GtkApplication* application = gtk_application_new(application_id, G_APPLICATION_DEFAULT_FLAGS);
    TimedLayerState state{application, timeout_seconds, 0};

    g_signal_connect(application, "activate", activate_callback, &state);
    const int status = g_application_run(G_APPLICATION(application), 0, nullptr);
    if (state.quit_timer_id != 0) g_source_remove(state.quit_timer_id);
    g_object_unref(application);
    return status;
}

// --- Public API ---

int run_test_layer(int timeout_seconds) {
    return run_timed_application("dev.realmheart.test-layer", timeout_seconds, G_CALLBACK(activate_test_layer));
}

int run_bar(int timeout_seconds) {
    return run_timed_application("dev.realmheart.bar", timeout_seconds, G_CALLBACK(activate_bar));
}

int run_sidebar(int timeout_seconds) {
    return run_timed_application("dev.realmheart.sidebar", timeout_seconds, G_CALLBACK(activate_sidebar));
}

int run_lockscreen_test(int timeout_seconds) {
    return run_timed_application("dev.realmheart.lockscreen-test", timeout_seconds, G_CALLBACK(activate_lockscreen_test));
}

} // namespace realmheart::ui
