#include "services/SessionManager.hpp"
#include "ui/powermenu/PowerMenuOverlay.hpp"

#include <glib-unix.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

struct RendererState {
    GtkApplication* application = nullptr;
    std::unique_ptr<realmheart::services::SessionManager> session;
    std::unique_ptr<realmheart::ui::powermenu::PowerMenuOverlay> overlay;
    GtkCssProvider* transparency_provider = nullptr;
    guint stdin_watch_id = 0;
    double origin_x = 24.0 / 1920.0;
    double origin_y = 1048.0 / 1080.0;
    int monitor_index = 0;
    bool close_requested = false;
};

bool parse_unit_double(const char* text, double& value) {
    if (text == nullptr || *text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = std::clamp(parsed, 0.0, 1.0);
    return true;
}

void install_transparency_css(RendererState& state) {
    GdkDisplay* display = gdk_display_get_default();
    if (display == nullptr) return;

    state.transparency_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        state.transparency_provider,
        R"CSS(
            window.realmheart-power-menu-window,
            window.realmheart-power-menu-window > *,
            .realmheart-power-menu-root,
            .realmheart-power-menu-scene,
            .realmheart-power-menu-media,
            .realmheart-power-menu-poster,
            .realmheart-power-menu-video,
            .realmheart-power-menu-ripple {
                background-color: rgba(0, 0, 0, 0);
                background-image: none;
                border: none;
                box-shadow: none;
            }
        )CSS"
    );
    gtk_style_context_add_provider_for_display(
        display,
        GTK_STYLE_PROVIDER(state.transparency_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER + 1
    );
}

void remove_transparency_css(RendererState& state) {
    if (state.transparency_provider == nullptr) return;
    if (GdkDisplay* display = gdk_display_get_default(); display != nullptr) {
        gtk_style_context_remove_provider_for_display(
            display,
            GTK_STYLE_PROVIDER(state.transparency_provider)
        );
    }
    g_clear_object(&state.transparency_provider);
}

void begin_close(RendererState& state) {
    if (state.close_requested) return;
    state.close_requested = true;
    if (state.overlay != nullptr) {
        state.overlay->hide();
    } else if (state.application != nullptr) {
        g_application_quit(G_APPLICATION(state.application));
    }
}

gboolean stdin_callback(gint fd, GIOCondition condition, gpointer data) {
    auto* state = static_cast<RendererState*>(data);
    if (state == nullptr) return G_SOURCE_REMOVE;

    char buffer[128]{};
    bool received_close = false;
    if ((condition & G_IO_IN) != 0) {
        while (true) {
            const ssize_t count = ::read(fd, buffer, sizeof(buffer));
            if (count > 0) {
                const std::string_view command(buffer, static_cast<std::size_t>(count));
                if (command.find("close") != std::string_view::npos) {
                    received_close = true;
                }
                if (count < static_cast<ssize_t>(sizeof(buffer))) break;
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count == 0) received_close = true;
            break;
        }
    }
    if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
        received_close = true;
    }

    if (!received_close) return G_SOURCE_CONTINUE;
    state->stdin_watch_id = 0;
    begin_close(*state);
    return G_SOURCE_REMOVE;
}

void activate(GtkApplication* application, gpointer data) {
    auto* state = static_cast<RendererState*>(data);
    if (state == nullptr || state->overlay != nullptr) return;

    install_transparency_css(*state);
    state->session = std::make_unique<realmheart::services::SessionManager>();
    state->overlay = std::make_unique<realmheart::ui::powermenu::PowerMenuOverlay>(
        application,
        realmheart::ui::powermenu::PowerMenuActions{
            .lock = [state] { return state->session->lock(); },
            .suspend = [state] { return state->session->suspend(); },
            .logout = [state] { return state->session->logout(); },
            .reboot = [state] { return state->session->reboot(); },
            .power_off = [state] { return state->session->power_off(); },
        },
        state->monitor_index
    );
    state->overlay->set_closed_callback([state] {
        if (state->application != nullptr) {
            g_application_quit(G_APPLICATION(state->application));
        }
    });
    state->overlay->show(state->origin_x, state->origin_y);
    if (state->close_requested) state->overlay->hide();
}

void print_usage() {
    std::cerr << "Usage: realmheart-power-menu-renderer "
              << "[--monitor-index N] [--origin-x 0..1] [--origin-y 0..1]\n";
}

} // namespace

int main(int argc, char** argv) {
    RendererState state;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--monitor-index") {
            if (index + 1 >= argc) { print_usage(); return 2; }
            try {
                state.monitor_index = std::max(std::stoi(argv[++index]), 0);
            } catch (...) {
                print_usage();
                return 2;
            }
            continue;
        }

        double* destination = nullptr;
        if (argument == "--origin-x") destination = &state.origin_x;
        else if (argument == "--origin-y") destination = &state.origin_y;
        else {
            print_usage();
            return 2;
        }

        if (index + 1 >= argc ||
            !parse_unit_double(argv[++index], *destination)) {
            print_usage();
            return 2;
        }
    }

    state.application = gtk_application_new(
        "dev.realmheart.power-menu-renderer",
        G_APPLICATION_NON_UNIQUE
    );
    g_signal_connect(state.application, "activate", G_CALLBACK(activate), &state);
    state.stdin_watch_id = g_unix_fd_add(
        STDIN_FILENO,
        static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
        stdin_callback,
        &state
    );

    const int status = g_application_run(
        G_APPLICATION(state.application),
        0,
        nullptr
    );

    if (state.stdin_watch_id != 0) {
        g_source_remove(state.stdin_watch_id);
        state.stdin_watch_id = 0;
    }
    state.overlay.reset();
    state.session.reset();
    remove_transparency_css(state);
    g_clear_object(&state.application);
    return status;
}
