#include "services/NotificationDaemon.hpp"

#include <iostream>
#include <string_view>

namespace realmheart::services {
namespace {

constexpr const char* kBusName = "org.freedesktop.Notifications";
constexpr const char* kObjectPath = "/org/freedesktop/Notifications";
constexpr const char* kInterface = "org.freedesktop.Notifications";

constexpr const char* kIntrospectionXml = R"xml(
<node>
  <interface name="org.freedesktop.Notifications">
    <method name="GetCapabilities">
      <arg direction="out" name="capabilities" type="as"/>
    </method>
    <method name="Notify">
      <arg direction="in" name="app_name" type="s"/>
      <arg direction="in" name="replaces_id" type="u"/>
      <arg direction="in" name="app_icon" type="s"/>
      <arg direction="in" name="summary" type="s"/>
      <arg direction="in" name="body" type="s"/>
      <arg direction="in" name="actions" type="as"/>
      <arg direction="in" name="hints" type="a{sv}"/>
      <arg direction="in" name="expire_timeout" type="i"/>
      <arg direction="out" name="id" type="u"/>
    </method>
    <method name="CloseNotification">
      <arg direction="in" name="id" type="u"/>
    </method>
    <method name="GetServerInformation">
      <arg direction="out" name="name" type="s"/>
      <arg direction="out" name="vendor" type="s"/>
      <arg direction="out" name="version" type="s"/>
      <arg direction="out" name="spec_version" type="s"/>
    </method>
    <signal name="NotificationClosed">
      <arg name="id" type="u"/>
      <arg name="reason" type="u"/>
    </signal>
    <signal name="ActionInvoked">
      <arg name="id" type="u"/>
      <arg name="action_key" type="s"/>
    </signal>
  </interface>
</node>
)xml";

} // namespace

NotificationDaemon::NotificationDaemon(NotificationServer& server, NotificationHistory& history)
    : server_(server), history_(history) {}

NotificationDaemon::~NotificationDaemon() {
    stop();
}

bool NotificationDaemon::start() {
    if (owner_id_ != 0) return true;

    GError* error = nullptr;
    node_info_ = g_dbus_node_info_new_for_xml(kIntrospectionXml, &error);
    if (node_info_ == nullptr) {
        std::cerr << "Notification DBus introspection parse failed: "
                  << (error != nullptr ? error->message : "unknown error") << '\n';
        g_clear_error(&error);
        return false;
    }

    owner_id_ = g_bus_own_name(
        G_BUS_TYPE_SESSION,
        kBusName,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired,
        on_name_acquired,
        on_name_lost,
        this,
        nullptr
    );
    return owner_id_ != 0;
}

void NotificationDaemon::stop() {
    history_.set_capture_active(false);
    if (owner_id_ != 0) {
        g_bus_unown_name(owner_id_);
        owner_id_ = 0;
    }
    if (connection_ != nullptr && registration_id_ != 0) {
        g_dbus_connection_unregister_object(connection_, registration_id_);
        registration_id_ = 0;
    }
    g_clear_object(&connection_);
    g_clear_pointer(&node_info_, g_dbus_node_info_unref);
}

void NotificationDaemon::on_bus_acquired(
    GDBusConnection* connection,
    const gchar*,
    gpointer user_data
) {
    static_cast<NotificationDaemon*>(user_data)->register_object(connection);
}

void NotificationDaemon::on_name_acquired(
    GDBusConnection*,
    const gchar*,
    gpointer user_data
) {
    static_cast<NotificationDaemon*>(user_data)->history_.set_capture_active(true);
}

void NotificationDaemon::on_name_lost(
    GDBusConnection*,
    const gchar*,
    gpointer user_data
) {
    static_cast<NotificationDaemon*>(user_data)->history_.set_capture_active(false);
}

void NotificationDaemon::on_method_call(
    GDBusConnection* connection,
    const gchar*,
    const gchar*,
    const gchar*,
    const gchar* method_name,
    GVariant* parameters,
    GDBusMethodInvocation* invocation,
    gpointer user_data
) {
    static_cast<NotificationDaemon*>(user_data)->handle_method_call(
        connection,
        method_name,
        parameters,
        invocation
    );
}

void NotificationDaemon::register_object(GDBusConnection* connection) {
    if (registration_id_ != 0 || node_info_ == nullptr) return;

    static const GDBusInterfaceVTable interface_vtable = {
        on_method_call,
        nullptr,
        nullptr,
        {},
    };

    GError* error = nullptr;
    registration_id_ = g_dbus_connection_register_object(
        connection,
        kObjectPath,
        node_info_->interfaces[0],
        &interface_vtable,
        this,
        nullptr,
        &error
    );
    if (registration_id_ == 0) {
        std::cerr << "Notification DBus object registration failed: "
                  << (error != nullptr ? error->message : "unknown error") << '\n';
        g_clear_error(&error);
        return;
    }

    connection_ = G_DBUS_CONNECTION(g_object_ref(connection));
}

void NotificationDaemon::handle_method_call(
    GDBusConnection* connection,
    const gchar* method_name,
    GVariant* parameters,
    GDBusMethodInvocation* invocation
) {
    const std::string_view method(method_name);
    if (method == "GetCapabilities") {
        const gchar* capabilities[] = {"body", nullptr};
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(@as)", g_variant_new_strv(capabilities, -1))
        );
        return;
    }

    if (method == "GetServerInformation") {
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(ssss)", "Realmheart", "Zahed", "0.1.0", "1.2")
        );
        return;
    }

    if (method == "Notify") {
        const gchar* app_name = nullptr;
        const gchar* app_icon = nullptr;
        const gchar* summary = nullptr;
        const gchar* body = nullptr;
        guint32 replaces_id = 0;
        GVariant* actions = nullptr;
        GVariant* hints = nullptr;
        gint32 expire_timeout = 0;
        g_variant_get(
            parameters,
            "(&su&s&s&s@as@a{sv}i)",
            &app_name,
            &replaces_id,
            &app_icon,
            &summary,
            &body,
            &actions,
            &hints,
            &expire_timeout
        );
        static_cast<void>(app_icon);
        static_cast<void>(expire_timeout);
        g_variant_unref(actions);
        g_variant_unref(hints);

        const auto id = server_.notify(app_name, replaces_id, summary, body);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
        return;
    }

    if (method == "CloseNotification") {
        guint32 id = 0;
        g_variant_get(parameters, "(u)", &id);
        if (server_.close(id)) emit_closed(connection, id, 3);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    g_dbus_method_invocation_return_dbus_error(
        invocation,
        "org.freedesktop.DBus.Error.UnknownMethod",
        "Unsupported notification method"
    );
}

void NotificationDaemon::emit_closed(
    GDBusConnection* connection,
    std::uint32_t id,
    std::uint32_t reason
) {
    g_dbus_connection_emit_signal(
        connection,
        nullptr,
        kObjectPath,
        kInterface,
        "NotificationClosed",
        g_variant_new("(uu)", id, reason),
        nullptr
    );
}

} // namespace realmheart::services
