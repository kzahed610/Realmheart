#pragma once

#include "services/NotificationServer.hpp"
#include "services/Notifications.hpp"

#include <gio/gio.h>

namespace realmheart::services {

class NotificationDaemon {
public:
    NotificationDaemon(NotificationServer& server, NotificationHistory& history);
    ~NotificationDaemon();

    NotificationDaemon(const NotificationDaemon&) = delete;
    NotificationDaemon& operator=(const NotificationDaemon&) = delete;

    bool start();
    void stop();

private:
    static void on_bus_acquired(
        GDBusConnection* connection,
        const gchar* name,
        gpointer user_data
    );
    static void on_name_acquired(
        GDBusConnection* connection,
        const gchar* name,
        gpointer user_data
    );
    static void on_name_lost(
        GDBusConnection* connection,
        const gchar* name,
        gpointer user_data
    );
    static void on_method_call(
        GDBusConnection* connection,
        const gchar* sender,
        const gchar* object_path,
        const gchar* interface_name,
        const gchar* method_name,
        GVariant* parameters,
        GDBusMethodInvocation* invocation,
        gpointer user_data
    );

    void register_object(GDBusConnection* connection);
    void handle_method_call(
        GDBusConnection* connection,
        const gchar* method_name,
        GVariant* parameters,
        GDBusMethodInvocation* invocation
    );
    void emit_closed(GDBusConnection* connection, std::uint32_t id, std::uint32_t reason);

    NotificationServer& server_;
    NotificationHistory& history_;
    guint owner_id_ = 0;
    guint registration_id_ = 0;
    GDBusConnection* connection_ = nullptr;
    GDBusNodeInfo* node_info_ = nullptr;
};

} // namespace realmheart::services
