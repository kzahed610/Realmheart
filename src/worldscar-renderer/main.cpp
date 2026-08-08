#include "worldscar/WallpaperLibrary.hpp"
#include "worldscar/WorldscarOverlay.hpp"
#include "worldscar/WorldscarProtocol.hpp"
#include "worldscar/WorldscarSelection.hpp"

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib-unix.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

struct Options {
    bool stdio = false;
    std::filesystem::path base;
    std::filesystem::path candidate;
    std::filesystem::path library_root;
};

struct RendererState {
    GtkApplication* application = nullptr;
    Options options;
    realmheart::worldscar::WallpaperLibrary library;
    realmheart::worldscar::WallpaperDiscovery discovery;
    std::unique_ptr<realmheart::worldscar::WorldscarOverlay> overlay;
    std::optional<realmheart::worldscar::WorldscarResult> result;
    std::string control_buffer;
    guint stdin_watch_id = 0;
    guint library_refresh_timeout_id = 0;
    GFileMonitor* library_monitor = nullptr;
    bool library_refresh_pending = false;
    bool application_held = false;
};

void print_usage() {
    std::cerr
        << "Usage:\n"
        << "  realmheart-worldscar-renderer --stdio [--library-root PATH]\n"
        << "  realmheart-worldscar-renderer --base PATH "
        << "[--candidate PATH] [--library-root PATH]\n";
}

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--stdio") {
            options.stdio = true;
            continue;
        }

        std::filesystem::path* destination = nullptr;
        if (argument == "--base") destination = &options.base;
        else if (argument == "--candidate") destination = &options.candidate;
        else if (argument == "--library-root") destination = &options.library_root;
        else {
            print_usage();
            return std::nullopt;
        }

        if (index + 1 >= argc) {
            print_usage();
            return std::nullopt;
        }
        *destination = argv[++index];
    }

    if (!options.stdio && options.base.empty()) {
        print_usage();
        return std::nullopt;
    }
    return options;
}

void refresh_library(RendererState& state) {
    if (state.overlay != nullptr && !state.overlay->active()) {
        state.overlay->invalidate_candidate_cache();
    }
    state.discovery = state.library.discover(state.options.library_root);
    if (state.overlay != nullptr && !state.overlay->active()) {
        state.overlay->prewarm_thumbnail_cache(state.discovery.paths);
    }
    for (const auto& diagnostic : state.discovery.diagnostics) {
        std::cerr << "[WorldscarLibrary] " << diagnostic << '\n';
    }
    std::cerr << "[WorldscarLibrary] warm index contains "
              << state.discovery.paths.size() << " wallpaper(s)\n";
}

gboolean deferred_library_refresh(gpointer data) {
    auto* state = static_cast<RendererState*>(data);
    if (state == nullptr) return G_SOURCE_REMOVE;
    state->library_refresh_timeout_id = 0;

    if (state->overlay != nullptr && state->overlay->active()) {
        // Never rescan/validate files in the middle of an interaction. The
        // pending flag is flushed as soon as the session reaches a terminal
        // state or the backend handoff completes.
        state->library_refresh_pending = true;
        return G_SOURCE_REMOVE;
    }

    state->library_refresh_pending = false;
    refresh_library(*state);
    return G_SOURCE_REMOVE;
}

void schedule_library_refresh(RendererState& state) {
    state.library_refresh_pending = true;
    if (state.library_refresh_timeout_id != 0) return;
    state.library_refresh_timeout_id = g_timeout_add(
        250,
        &deferred_library_refresh,
        &state
    );
}

void flush_pending_library_refresh(RendererState& state) {
    if (!state.library_refresh_pending ||
        state.library_refresh_timeout_id != 0) {
        return;
    }
    schedule_library_refresh(state);
}

void install_library_monitor(RendererState& state) {
    if (state.library_monitor != nullptr) return;

    const auto root = state.options.library_root.empty()
        ? realmheart::worldscar::WallpaperLibrary::default_root()
        : state.options.library_root;
    GFile* directory = g_file_new_for_path(root.c_str());
    if (directory == nullptr) return;

    GError* error = nullptr;
    state.library_monitor = g_file_monitor_directory(
        directory,
        G_FILE_MONITOR_WATCH_MOVES,
        nullptr,
        &error
    );
    g_object_unref(directory);

    if (state.library_monitor == nullptr) {
        std::cerr << "[WorldscarLibrary] directory watch unavailable: "
                  << (error != nullptr && error->message != nullptr
                      ? error->message
                      : "unknown error")
                  << '\n';
        g_clear_error(&error);
        return;
    }
    g_clear_error(&error);

    g_signal_connect(
        state.library_monitor,
        "changed",
        G_CALLBACK(+[](
            GFileMonitor*,
            GFile*,
            GFile*,
            GFileMonitorEvent,
            gpointer data
        ) {
            auto* monitor_state = static_cast<RendererState*>(data);
            if (monitor_state != nullptr) {
                schedule_library_refresh(*monitor_state);
            }
        }),
        &state
    );
}

void clear_library_monitor(RendererState& state) {
    if (state.library_refresh_timeout_id != 0) {
        g_source_remove(state.library_refresh_timeout_id);
        state.library_refresh_timeout_id = 0;
    }
    if (state.library_monitor != nullptr) {
        g_file_monitor_cancel(state.library_monitor);
        g_clear_object(&state.library_monitor);
    }
    state.library_refresh_pending = false;
}

std::optional<realmheart::worldscar::WorldscarSelection> resolve_selection(
    const RendererState& state,
    const std::filesystem::path& current,
    std::string& error
) {
    if (!state.options.candidate.empty()) {
        const std::vector<std::filesystem::path> forced{state.options.candidate};
        return realmheart::worldscar::WorldscarSelection::create(forced, {});
    }

    if (state.discovery.paths.empty()) {
        const auto root = state.options.library_root.empty()
            ? realmheart::worldscar::WallpaperLibrary::default_root()
            : state.options.library_root;
        error = "no supported wallpapers were found in " + root.string();
        return std::nullopt;
    }

    const auto selection = realmheart::worldscar::WorldscarSelection::create(
        state.discovery.paths,
        current
    );
    if (!selection) {
        error = "unable to create Worldscar wallpaper selection";
        return std::nullopt;
    }
    return selection;
}

void write_result(const realmheart::worldscar::WorldscarResult& result) {
    std::cout << realmheart::worldscar::serialize_worldscar_result(result)
              << std::flush;
}

void stop_stdio(RendererState& state) {
    if (state.stdin_watch_id != 0) {
        g_source_remove(state.stdin_watch_id);
        state.stdin_watch_id = 0;
    }
    if (state.application_held && state.application != nullptr) {
        g_application_release(G_APPLICATION(state.application));
        state.application_held = false;
    }
}

void handle_command(
    RendererState& state,
    const realmheart::worldscar::WorldscarCommand& command
) {
    using realmheart::worldscar::WorldscarCommandKind;
    using realmheart::worldscar::WorldscarResult;
    using realmheart::worldscar::WorldscarResultKind;

    if (state.overlay == nullptr) {
        write_result(WorldscarResult{
            WorldscarResultKind::Error,
            "Worldscar overlay is unavailable"
        });
        return;
    }

    switch (command.kind) {
    case WorldscarCommandKind::Prepare: {
        if (state.overlay->active()) return;

        std::string selection_error;
        const auto selection = resolve_selection(
            state,
            command.payload,
            selection_error
        );
        if (!selection) {
            std::cerr << "[Worldscar] preview preload skipped: "
                      << selection_error << '\n';
            return;
        }

        std::string preload_error;
        if (!state.overlay->preload_preview(*selection, &preload_error)) {
            std::cerr << "[Worldscar] preview preload failed: "
                      << preload_error << '\n';
            return;
        }

        std::cerr << "[Worldscar] selected preview preload started: "
                  << selection->selected() << '\n';
        return;
    }

    case WorldscarCommandKind::Open: {
        if (state.overlay->active()) {
            write_result(WorldscarResult{
                WorldscarResultKind::Error,
                "Worldscar session is already active"
            });
            return;
        }

        std::string selection_error;
        auto selection = resolve_selection(
            state,
            command.payload,
            selection_error
        );
        if (!selection) {
            write_result(WorldscarResult{
                WorldscarResultKind::Error,
                std::move(selection_error)
            });
            return;
        }

        const auto initial_selected = selection->selected();
        const auto preview = selection->preview();
        std::string show_error;
        if (!state.overlay->show(std::move(*selection), &show_error)) {
            write_result(WorldscarResult{
                WorldscarResultKind::Error,
                std::move(show_error)
            });
            return;
        }

        std::cerr << "[Worldscar] warm session opened\n"
                  << "[Worldscar] current=" << command.payload << '\n'
                  << "[Worldscar] previous="
                  << (preview.previous_visible ? preview.previous.string() : "<hidden>") << '\n'
                  << "[Worldscar] selected=" << initial_selected << '\n'
                  << "[Worldscar] next="
                  << (preview.next_visible ? preview.next.string() : "<hidden>") << '\n';
        return;
    }

    case WorldscarCommandKind::Close:
        state.overlay->cancel();
        return;

    case WorldscarCommandKind::ApplyPrepared:
        state.overlay->backend_prepared();
        return;

    case WorldscarCommandKind::ApplyCommitted:
        // The backend reveal is complete, but Worldscar still owns the final
        // purple/gold slash fade. Library refresh waits for COMPLETE so it can
        // never race that last visual endpoint.
        state.overlay->backend_committed();
        return;

    case WorldscarCommandKind::ApplyFailed:
        state.overlay->backend_failed(command.payload);
        return;

    case WorldscarCommandKind::Refresh:
        if (!state.overlay->active()) {
            state.library_refresh_pending = false;
            refresh_library(state);
        } else {
            state.library_refresh_pending = true;
        }
        std::cout << "READY\n" << std::flush;
        return;
    }
}

gboolean stdin_ready_callback(
    gint fd,
    GIOCondition condition,
    gpointer data
) {
    auto* state = static_cast<RendererState*>(data);
    if (state == nullptr) return G_SOURCE_REMOVE;

    if ((condition & (G_IO_ERR | G_IO_NVAL)) != 0) {
        state->stdin_watch_id = 0;
        stop_stdio(*state);
        g_application_quit(G_APPLICATION(state->application));
        return G_SOURCE_REMOVE;
    }

    std::array<char, 4096> buffer{};
    ssize_t count = -1;
    do {
        count = ::read(fd, buffer.data(), buffer.size());
    } while (count < 0 && errno == EINTR);

    if (count > 0) {
        state->control_buffer.append(
            buffer.data(),
            static_cast<std::size_t>(count)
        );
    } else if (count == 0) {
        // We are executing inside this source; clear the id before the common
        // shutdown helper so it does not attempt to remove itself.
        state->stdin_watch_id = 0;
        stop_stdio(*state);
        g_application_quit(G_APPLICATION(state->application));
        return G_SOURCE_REMOVE;
    } else {
        std::cerr << "[Worldscar] control channel read failed: "
                  << std::strerror(errno) << '\n';
        state->stdin_watch_id = 0;
        stop_stdio(*state);
        g_application_quit(G_APPLICATION(state->application));
        return G_SOURCE_REMOVE;
    }

    std::size_t newline = 0;
    while ((newline = state->control_buffer.find('\n')) != std::string::npos) {
        std::string line = state->control_buffer.substr(0, newline + 1);
        state->control_buffer.erase(0, newline + 1);

        const auto command =
            realmheart::worldscar::parse_worldscar_command(line);
        if (!command) {
            write_result({
                realmheart::worldscar::WorldscarResultKind::Error,
                "invalid Worldscar control command"
            });
            continue;
        }
        handle_command(*state, *command);
    }

    if ((condition & G_IO_HUP) != 0) {
        state->stdin_watch_id = 0;
        stop_stdio(*state);
        g_application_quit(G_APPLICATION(state->application));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void activate(GtkApplication* application, gpointer data) {
    auto* state = static_cast<RendererState*>(data);
    if (state == nullptr || state->overlay != nullptr) return;

    refresh_library(*state);
    if (state->options.stdio) install_library_monitor(*state);

    state->overlay = std::make_unique<realmheart::worldscar::WorldscarOverlay>(
        application,
        [state](realmheart::worldscar::WorldscarResult result) {
            using realmheart::worldscar::WorldscarResultKind;
            if (state->options.stdio) {
                write_result(result);
                const bool terminal =
                    result.kind != WorldscarResultKind::Apply &&
                    result.kind != WorldscarResultKind::Commit;
                if (terminal) flush_pending_library_refresh(*state);
                return;
            }

            // One-shot/manual mode has no real wallpaper backend. Exercise both
            // sides of the helper state machine locally so lifecycle tests can
            // still complete without pretending a thumbnail became fullscreen.
            if (result.kind == WorldscarResultKind::Apply &&
                state->overlay != nullptr) {
                state->overlay->backend_prepared();
                return;
            }
            if (result.kind == WorldscarResultKind::Commit &&
                state->overlay != nullptr) {
                state->overlay->backend_committed();
                return;
            }

            state->result = result;
            if (state->application != nullptr) {
                g_application_quit(G_APPLICATION(state->application));
            }
        }
    );

    std::string prepare_error;
    if (!state->overlay->prepare(&prepare_error)) {
        state->result = realmheart::worldscar::WorldscarResult{
            realmheart::worldscar::WorldscarResultKind::Error,
            std::move(prepare_error)
        };
        if (state->options.stdio) write_result(*state->result);
        g_application_quit(G_APPLICATION(application));
        return;
    }

    // Start building the persistent raw thumbnail cache immediately while the
    // warm helper is otherwise idle. One serial worker handles the full
    // library and automatically pauses once an interactive session begins.
    state->overlay->prewarm_thumbnail_cache(state->discovery.paths);

    if (state->options.stdio) {
        g_application_hold(G_APPLICATION(application));
        state->application_held = true;
        state->stdin_watch_id = g_unix_fd_add(
            STDIN_FILENO,
            static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
            &stdin_ready_callback,
            state
        );
        std::cout << "READY\n" << std::flush;
        std::cerr << "[Worldscar] warm helper ready\n";
        return;
    }

    std::string selection_error;
    auto selection = resolve_selection(
        *state,
        state->options.base,
        selection_error
    );
    if (!selection) {
        state->result = realmheart::worldscar::WorldscarResult{
            realmheart::worldscar::WorldscarResultKind::Error,
            std::move(selection_error)
        };
        g_application_quit(G_APPLICATION(application));
        return;
    }

    const auto initial_selected = selection->selected();
    std::string error;
    if (!state->overlay->show(std::move(*selection), &error)) {
        state->result = realmheart::worldscar::WorldscarResult{
            realmheart::worldscar::WorldscarResultKind::Error,
            std::move(error)
        };
        g_application_quit(G_APPLICATION(application));
        return;
    }

    std::cerr << "[Worldscar] reference session opened\n"
              << "[Worldscar] current=" << state->options.base << '\n'
              << "[Worldscar] selected=" << initial_selected << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    if (!options) return 2;

    RendererState state;
    state.options = *options;
    state.application = gtk_application_new(
        "dev.realmheart.worldscar-renderer",
        G_APPLICATION_NON_UNIQUE
    );
    g_signal_connect(state.application, "activate", G_CALLBACK(activate), &state);

    const int application_status = g_application_run(
        G_APPLICATION(state.application),
        0,
        nullptr
    );

    stop_stdio(state);
    clear_library_monitor(state);
    state.overlay.reset();
    g_clear_object(&state.application);

    if (state.options.stdio) {
        return application_status;
    }

    if (!state.result) {
        state.result = realmheart::worldscar::WorldscarResult{
            realmheart::worldscar::WorldscarResultKind::Error,
            application_status == 0
                ? "Worldscar helper exited without a result"
                : "Worldscar GTK application failed"
        };
    }

    std::cout << realmheart::worldscar::serialize_worldscar_result(*state.result)
              << std::flush;
    return state.result->kind == realmheart::worldscar::WorldscarResultKind::Error
        ? 1
        : application_status;
}
