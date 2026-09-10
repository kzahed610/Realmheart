#include "services/NotificationDaemon.hpp"
#include "services/NotificationServer.hpp"
#include "services/Notifications.hpp"

#include <gio/gio.h>

#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct ClosedSignalState {
    std::mutex mutex;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> events;
};

void on_closed_signal(
    GDBusConnection*,
    const gchar*,
    const gchar*,
    const gchar*,
    const gchar*,
    GVariant* parameters,
    gpointer user_data
) {
    guint32 id = 0;
    guint32 reason = 0;
    g_variant_get(parameters, "(uu)", &id, &reason);
    auto* state = static_cast<ClosedSignalState*>(user_data);
    std::lock_guard lock(state->mutex);
    state->events.emplace_back(id, reason);
}

bool has_closed_signal(ClosedSignalState& state, std::uint32_t id, std::uint32_t reason) {
    std::lock_guard lock(state.mutex);
    for (const auto& event : state.events) {
        if (event == std::pair{id, reason}) return true;
    }
    return false;
}

} // namespace

int main() {
    GMainLoop* loop = nullptr;
    std::thread loop_thread;
    try {
        realmheart::services::NotificationHistory history;
        realmheart::services::NotificationServer server(history);
        realmheart::services::NotificationDaemon daemon(server, history);
        require(daemon.start(), "daemon should begin bus ownership");

        loop = g_main_loop_new(nullptr, FALSE);
        loop_thread = std::thread([loop] { g_main_loop_run(loop); });

        bool acquired = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (history.snapshot().capture_active) {
                acquired = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(acquired, "daemon should acquire org.freedesktop.Notifications");

        GError* error = nullptr;
        GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        require(connection != nullptr, error != nullptr ? error->message : "session bus unavailable");
        g_clear_error(&error);

        GVariantBuilder actions;
        g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
        GVariantBuilder hints;
        g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
        GVariant* reply = g_dbus_connection_call_sync(
            connection,
            "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications",
            "Notify",
            g_variant_new(
                "(susssasa{sv}i)",
                "realmheart-test",
                0u,
                "",
                "DBus summary",
                "DBus body",
                &actions,
                &hints,
                120
            ),
            G_VARIANT_TYPE("(u)"),
            G_DBUS_CALL_FLAGS_NONE,
            2000,
            nullptr,
            &error
        );
        require(reply != nullptr, error != nullptr ? error->message : "Notify failed");
        g_clear_error(&error);

        guint32 id = 0;
        g_variant_get(reply, "(u)", &id);
        g_variant_unref(reply);
        require(id != 0, "Notify should return a non-zero id");
        require(server.contains(id), "new notification must be active before expiry");

        ClosedSignalState closed_signal;
        const guint subscription = g_dbus_connection_signal_subscribe(
            connection,
            "org.freedesktop.Notifications",
            "org.freedesktop.Notifications",
            "NotificationClosed",
            "/org/freedesktop/Notifications",
            nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_closed_signal,
            &closed_signal,
            nullptr
        );
        require(subscription != 0, "NotificationClosed signal subscription must succeed");

        auto snapshot = history.snapshot();
        require(snapshot.entries.size() == 1, "Notify should enter history");
        require(snapshot.entries.front().summary == "DBus summary", "Notify summary should survive DBus");

        bool expired = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (!server.contains(id) && has_closed_signal(closed_signal, id, 1)) {
                expired = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(expired, "expired notification must leave active state and emit reason 1");
        require(!server.contains(id), "expired notification must be inactive");
        require(has_closed_signal(closed_signal, id, 1), "expiry must emit NotificationClosed reason 1");
        snapshot = history.snapshot();
        require(snapshot.entries.size() == 1, "toast expiration must preserve sidebar history");

        reply = g_dbus_connection_call_sync(
            connection,
            "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications",
            "CloseNotification",
            g_variant_new("(u)", id),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            2000,
            nullptr,
            &error
        );
        require(reply == nullptr, "CloseNotification must reject an expired id");
        require(error != nullptr, "invalid CloseNotification must return a D-Bus error");
        g_clear_error(&error);
        require(history.snapshot().entries.size() == 1, "CloseNotification must preserve sidebar history");
        g_dbus_connection_signal_unsubscribe(connection, subscription);

        g_object_unref(connection);
        daemon.stop();
        g_main_loop_quit(loop);
        loop_thread.join();
        g_main_loop_unref(loop);
    } catch (const std::exception& error) {
        if (loop != nullptr) g_main_loop_quit(loop);
        if (loop_thread.joinable()) loop_thread.join();
        if (loop != nullptr) g_main_loop_unref(loop);
        std::cerr << "NotificationDaemonTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "NotificationDaemonTests passed\n";
    return 0;
}
