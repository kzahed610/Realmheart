#include "services/NotificationDaemon.hpp"
#include "services/NotificationServer.hpp"
#include "services/Notifications.hpp"

#include <gio/gio.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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
                5000
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

        const auto snapshot = history.snapshot();
        require(snapshot.entries.size() == 1, "Notify should enter history");
        require(snapshot.entries.front().summary == "DBus summary", "Notify summary should survive DBus");

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
        require(reply != nullptr, error != nullptr ? error->message : "CloseNotification failed");
        g_clear_error(&error);
        g_variant_unref(reply);
        require(history.snapshot().entries.empty(), "CloseNotification should remove history entry");

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
