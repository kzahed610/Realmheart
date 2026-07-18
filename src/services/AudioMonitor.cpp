#include "services/AudioMonitor.hpp"

#include "core/TaskExecutor.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace realmheart::services {
namespace {

constexpr guint kEventDebounceMs = 55;
constexpr guint kFallbackPollMs = 250;
constexpr guint kRestartDelayMs = 1000;
constexpr double kVolumeChangeEpsilonPercent = 0.20;

struct RefreshPayload;

bool is_sink_change_event(std::string_view line) {
    return line.find("Event 'change'") != std::string_view::npos &&
        line.find(" on sink #") != std::string_view::npos;
}

} // namespace

struct AudioMonitor::State {
    explicit State(ChangedCallback changed_callback)
        : callback(std::move(changed_callback)) {}

    std::atomic<bool> running{false};
    std::atomic<bool> refresh_in_flight{false};
    std::atomic<bool> refresh_pending{false};
    std::atomic<bool> notify_pending{false};

    ChangedCallback callback;

    GSubprocess* process = nullptr;
    GDataInputStream* stream = nullptr;
    GCancellable* cancellable = nullptr;

    guint debounce_id = 0;
    guint fallback_poll_id = 0;
    guint restart_id = 0;

    bool seeded = false;
    bool force_next_notification = false;
    double last_percent = 0.0;
    bool last_muted = false;
};

namespace {

struct RefreshPayload {
    std::shared_ptr<AudioMonitor::State> state;
    std::optional<AudioState> audio;
    bool notify = false;
};

void request_refresh(const std::shared_ptr<AudioMonitor::State>& state, bool notify);
void start_event_stream(const std::shared_ptr<AudioMonitor::State>& state);
void read_next_event(const std::shared_ptr<AudioMonitor::State>& state);

void clear_event_stream(
    const std::shared_ptr<AudioMonitor::State>& state,
    bool terminate
) {
    if (state->cancellable != nullptr) {
        g_cancellable_cancel(state->cancellable);
        g_object_unref(state->cancellable);
        state->cancellable = nullptr;
    }
    if (state->stream != nullptr) {
        g_object_unref(state->stream);
        state->stream = nullptr;
    }
    if (state->process != nullptr) {
        if (terminate) g_subprocess_force_exit(state->process);
        g_object_unref(state->process);
        state->process = nullptr;
    }
}

void start_fallback_poll(const std::shared_ptr<AudioMonitor::State>& state) {
    if (!state->running.load() || state->fallback_poll_id != 0) return;

    state->fallback_poll_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        kFallbackPollMs,
        +[](gpointer raw) -> gboolean {
            const auto& state = *static_cast<std::shared_ptr<AudioMonitor::State>*>(raw);
            if (!state->running.load()) {
                state->fallback_poll_id = 0;
                return G_SOURCE_REMOVE;
            }
            request_refresh(state, true);
            return G_SOURCE_CONTINUE;
        },
        new std::shared_ptr<AudioMonitor::State>(state),
        +[](gpointer raw) {
            delete static_cast<std::shared_ptr<AudioMonitor::State>*>(raw);
        }
    );
}

void schedule_restart(const std::shared_ptr<AudioMonitor::State>& state) {
    if (!state->running.load() || state->restart_id != 0) return;

    state->restart_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        kRestartDelayMs,
        +[](gpointer raw) -> gboolean {
            const auto& state = *static_cast<std::shared_ptr<AudioMonitor::State>*>(raw);
            state->restart_id = 0;
            if (!state->running.load()) return G_SOURCE_REMOVE;
            start_event_stream(state);
            return G_SOURCE_REMOVE;
        },
        new std::shared_ptr<AudioMonitor::State>(state),
        +[](gpointer raw) {
            delete static_cast<std::shared_ptr<AudioMonitor::State>*>(raw);
        }
    );
}

void apply_refresh_result(RefreshPayload* payload) {
    const auto& state = payload->state;

    if (state->running.load() && payload->audio) {
        const double percent = std::clamp(payload->audio->volume * 100.0, 0.0, 100.0);
        const bool was_seeded = state->seeded;
        const bool changed = !was_seeded ||
            std::abs(percent - state->last_percent) > kVolumeChangeEpsilonPercent ||
            payload->audio->muted != state->last_muted;

        state->seeded = true;
        state->last_percent = percent;
        state->last_muted = payload->audio->muted;

        if (payload->notify &&
            (changed || state->force_next_notification) &&
            state->callback) {
            state->callback(*payload->audio);
        }
        if (payload->notify) state->force_next_notification = false;
    }

    state->refresh_in_flight.store(false);
    if (state->running.load() && state->refresh_pending.exchange(false)) {
        request_refresh(state, false);
    }
}

void request_refresh(const std::shared_ptr<AudioMonitor::State>& state, bool notify) {
    if (!state->running.load()) return;
    if (notify) {
        state->notify_pending.store(true);
        if (!state->seeded) state->force_next_notification = true;
    }

    if (state->refresh_in_flight.exchange(true)) {
        state->refresh_pending.store(true);
        return;
    }

    const bool should_notify = state->notify_pending.exchange(false);
    const bool queued = realmheart::core::shared_task_executor().post(
        [state, should_notify] {
            if (!state->running.load()) {
                state->refresh_in_flight.store(false);
                return;
            }

            realmheart::core::CommandOptions options;
            options.deadline = std::chrono::milliseconds(650);
            auto audio = Audio::read_default_sink(options);

            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    apply_refresh_result(static_cast<RefreshPayload*>(raw));
                    return G_SOURCE_REMOVE;
                },
                new RefreshPayload{state, std::move(audio), should_notify},
                +[](gpointer raw) {
                    delete static_cast<RefreshPayload*>(raw);
                }
            );
        }
    );

    if (!queued) state->refresh_in_flight.store(false);
}

void schedule_event_refresh(const std::shared_ptr<AudioMonitor::State>& state) {
    if (!state->running.load()) return;
    state->notify_pending.store(true);
    if (!state->seeded) state->force_next_notification = true;

    if (state->debounce_id != 0) {
        g_source_remove(state->debounce_id);
        state->debounce_id = 0;
    }

    state->debounce_id = g_timeout_add_full(
        G_PRIORITY_DEFAULT,
        kEventDebounceMs,
        +[](gpointer raw) -> gboolean {
            const auto& state = *static_cast<std::shared_ptr<AudioMonitor::State>*>(raw);
            state->debounce_id = 0;
            request_refresh(state, false);
            return G_SOURCE_REMOVE;
        },
        new std::shared_ptr<AudioMonitor::State>(state),
        +[](gpointer raw) {
            delete static_cast<std::shared_ptr<AudioMonitor::State>*>(raw);
        }
    );
}

void event_line_ready(GObject* source, GAsyncResult* result, gpointer raw) {
    std::unique_ptr<std::shared_ptr<AudioMonitor::State>> holder(
        static_cast<std::shared_ptr<AudioMonitor::State>*>(raw)
    );
    const auto state = *holder;

    GError* error = nullptr;
    gsize length = 0;
    gchar* line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(source),
        result,
        &length,
        &error
    );

    if (line != nullptr) {
        if (state->running.load() && is_sink_change_event(
                std::string_view(line, static_cast<std::size_t>(length)))) {
            schedule_event_refresh(state);
        }
        g_free(line);

        if (state->running.load()) read_next_event(state);
        g_clear_error(&error);
        return;
    }

    const bool cancelled = error != nullptr &&
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);

    if (!state->running.load() || cancelled) return;

    clear_event_stream(state, false);
    schedule_restart(state);
}

void read_next_event(const std::shared_ptr<AudioMonitor::State>& state) {
    if (!state->running.load() || state->stream == nullptr ||
        state->cancellable == nullptr) {
        return;
    }

    g_data_input_stream_read_line_async(
        state->stream,
        G_PRIORITY_DEFAULT,
        state->cancellable,
        event_line_ready,
        new std::shared_ptr<AudioMonitor::State>(state)
    );
}

void start_event_stream(const std::shared_ptr<AudioMonitor::State>& state) {
    if (!state->running.load() || state->process != nullptr) return;

    gchar* pactl = g_find_program_in_path("pactl");
    if (pactl == nullptr) {
        start_fallback_poll(state);
        return;
    }

    GError* error = nullptr;
    state->process = g_subprocess_new(
        static_cast<GSubprocessFlags>(
            G_SUBPROCESS_FLAGS_STDOUT_PIPE |
            G_SUBPROCESS_FLAGS_STDERR_SILENCE
        ),
        &error,
        pactl,
        "subscribe",
        nullptr
    );
    g_free(pactl);

    if (state->process == nullptr) {
        g_clear_error(&error);
        start_fallback_poll(state);
        return;
    }

    state->cancellable = g_cancellable_new();
    state->stream = G_DATA_INPUT_STREAM(g_data_input_stream_new(
        g_subprocess_get_stdout_pipe(state->process)
    ));
    g_data_input_stream_set_newline_type(
        state->stream,
        G_DATA_STREAM_NEWLINE_TYPE_ANY
    );
    read_next_event(state);
}

} // namespace

AudioMonitor::AudioMonitor(ChangedCallback callback)
    : state_(std::make_shared<State>(std::move(callback))) {}

AudioMonitor::~AudioMonitor() {
    stop();
}

void AudioMonitor::start() {
    if (!state_ || state_->running.exchange(true)) return;

    state_->seeded = false;
    state_->force_next_notification = false;
    state_->refresh_pending.store(false);
    state_->notify_pending.store(false);

    // Establish a baseline without displaying anything. A hardware event that
    // races this seed is retained as a pending notified refresh.
    request_refresh(state_, false);
    start_event_stream(state_);
}

void AudioMonitor::stop() {
    if (!state_ || !state_->running.exchange(false)) return;

    state_->callback = {};

    if (state_->debounce_id != 0) {
        g_source_remove(state_->debounce_id);
        state_->debounce_id = 0;
    }
    if (state_->fallback_poll_id != 0) {
        g_source_remove(state_->fallback_poll_id);
        state_->fallback_poll_id = 0;
    }
    if (state_->restart_id != 0) {
        g_source_remove(state_->restart_id);
        state_->restart_id = 0;
    }

    clear_event_stream(state_, true);
}

} // namespace realmheart::services
