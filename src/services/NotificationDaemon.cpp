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
    : server_(server), history_(history) {
    server_.set_closed_handler([this](std::uint32_t id, std::uint32_t reason) {
        cancel_expiration(id);
        if (connection_ != nullptr) emit_closed(connection_, id, reason);
    });
}

NotificationDaemon::~NotificationDaemon() {
    server_.set_closed_handler({});
    stop();
    if (context_ != nullptr) {
        g_main_context_unref(context_);
        context_ = nullptr;
    }
}

bool NotificationDaemon::start() {
    if (owner_id_ != 0) return true;

    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    if (owner_id_ != 0) return true;
    stopping_ = false;
    if (context_ == nullptr) context_ = g_main_context_ref_thread_default();

    GError* error = nullptr;
    node_info_ = g_dbus_node_info_new_for_xml(kIntrospectionXml, &error);
    if (node_info_ == nullptr) {
        std::cerr << "Notification DBus introspection parse failed: "
                  << (error != nullptr ? error->message : "unknown error") << '\n';
        g_clear_error(&error);
        g_clear_pointer(&context_, g_main_context_unref);
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
    if (owner_id_ == 0) {
        g_clear_pointer(&node_info_, g_dbus_node_info_unref);
        g_clear_pointer(&context_, g_main_context_unref);
        return false;
    }
    return true;
}

void NotificationDaemon::stop() {
    GMainContext* context = nullptr;
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        context = context_;
        if (owner_id_ == 0 && node_info_ == nullptr) return;
    }

    if (context == nullptr || g_main_context_is_owner(context)) {
        stop_on_context();
        return;
    }

    struct StopRequest {
        explicit StopRequest(NotificationDaemon* owner) : daemon(owner) {}
        NotificationDaemon* daemon = nullptr;
        std::mutex mutex;
        std::condition_variable condition;
        bool complete = false;
    } request(this);
    g_main_context_invoke_full(
        context,
        G_PRIORITY_DEFAULT,
        +[](gpointer raw) -> gboolean {
            auto* request = static_cast<StopRequest*>(raw);
            request->daemon->stop_on_context();
            {
                std::lock_guard lock(request->mutex);
                request->complete = true;
            }
            request->condition.notify_one();
            return G_SOURCE_REMOVE;
        },
        &request,
        nullptr
    );
    std::unique_lock lock(request.mutex);
    request.condition.wait(lock, [&request] { return request.complete; });
}

void NotificationDaemon::stop_on_context() {
    for (const auto& [_, source] : expiration_sources_) {
        if (source != 0) g_source_remove(source);
    }
    expiration_sources_.clear();
    history_.set_capture_active(false);
    if (owner_id_ != 0) {
        g_bus_unown_name(owner_id_);
        owner_id_ = 0;
    }
    if (connection_ != nullptr && registration_id_ != 0) {
        g_dbus_connection_unregister_object(connection_, registration_id_);
    }
    registration_id_ = 0;
    reset_connection();
    g_clear_pointer(&node_info_, g_dbus_node_info_unref);
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    stopping_ = true;
    GMainContext* context = std::exchange(context_, nullptr);
    if (context != nullptr) g_main_context_unref(context);
}

void NotificationDaemon::on_bus_acquired(
    GDBusConnection* connection,
    const gchar*,
    gpointer user_data
) {
    static_cast<NotificationDaemon*>(user_data)->register_object(connection);
}

void NotificationDaemon::on_name_acquired(
    GDBusConnection* connection,
    const gchar*,
    gpointer user_data
) {
    auto* daemon = static_cast<NotificationDaemon*>(user_data);
    if (!daemon->stopping_ && daemon->connection_ == connection &&
        daemon->registration_id_ != 0) {
        daemon->history_.set_capture_active(true);
    }
}

void NotificationDaemon::on_name_lost(
    GDBusConnection* connection,
    const gchar*,
    gpointer user_data
) {
    auto* daemon = static_cast<NotificationDaemon*>(user_data);
    daemon->history_.set_capture_active(false);
    if (!daemon->stopping_) daemon->reset_connection(connection);
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
    if (stopping_ || connection == nullptr || node_info_ == nullptr) return;

    // A bus-acquired callback is a fresh registration opportunity. The old
    // registration ID is scoped to the old connection and must never gate the
    // new one.
    reset_connection();

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

void NotificationDaemon::reset_connection(GDBusConnection* connection) {
    if (connection != nullptr && connection_ != connection) return;
    if (connection_ != nullptr && registration_id_ != 0) {
        g_dbus_connection_unregister_object(connection_, registration_id_);
    }
    registration_id_ = 0;
    g_clear_object(&connection_);
}

void NotificationDaemon::handle_method_call(
    GDBusConnection* connection,
    const gchar* method_name,
    GVariant* parameters,
    GDBusMethodInvocation* invocation
) {
    static_cast<void>(connection);
    if (parameters == nullptr || method_name == nullptr) {
        g_dbus_method_invocation_return_dbus_error(
            invocation,
            "org.freedesktop.DBus.Error.InvalidArgs",
            "Malformed notification method call"
        );
        return;
    }
    const std::string_view method(method_name);
    if (method == "GetCapabilities") {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("()"))) {
            g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.freedesktop.DBus.Error.InvalidArgs",
                "GetCapabilities expects no arguments"
            );
            return;
        }
        const gchar* capabilities[] = {"body", nullptr};
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(@as)", g_variant_new_strv(capabilities, -1))
        );
        return;
    }

    if (method == "GetServerInformation") {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("()"))) {
            g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.freedesktop.DBus.Error.InvalidArgs",
                "GetServerInformation expects no arguments"
            );
            return;
        }
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(ssss)", "Realmheart", "Zahed", "0.1.0", "1.2")
        );
        return;
    }

    if (method == "Notify") {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(susssasa{sv}i)"))) {
            g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.freedesktop.DBus.Error.InvalidArgs",
                "Notify arguments have an invalid signature"
            );
            return;
        }
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
        g_variant_unref(actions);
        g_variant_unref(hints);

        const int effective_timeout = expire_timeout < 0 ? 5000 : expire_timeout;
        const auto id = server_.notify(
            app_name,
            replaces_id,
            summary,
            body,
            effective_timeout
        );
        cancel_expiration(id);
        if (effective_timeout > 0) schedule_expiration(id, effective_timeout);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
        return;
    }

    if (method == "CloseNotification") {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(u)"))) {
            g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.freedesktop.DBus.Error.InvalidArgs",
                "CloseNotification expects one notification id"
            );
            return;
        }
        guint32 id = 0;
        g_variant_get(parameters, "(u)", &id);
        if (!server_.close(id, 3)) {
            g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.freedesktop.DBus.Error.InvalidArgs",
                "Notification id is not active"
            );
            return;
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    g_dbus_method_invocation_return_dbus_error(
        invocation,
        "org.freedesktop.DBus.Error.UnknownMethod",
        "Unsupported notification method"
    );
}

void NotificationDaemon::schedule_expiration(std::uint32_t id, int timeout_ms) {
    if (timeout_ms <= 0) return;
    struct Expiration {
        NotificationDaemon* daemon;
        std::uint32_t id;
    };
    GSource* source = g_timeout_source_new(static_cast<guint>(timeout_ms));
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(
        source,
        +[](gpointer raw) -> gboolean {
            auto* expiration = static_cast<Expiration*>(raw);
            auto* daemon = expiration->daemon;
            daemon->expiration_sources_.erase(expiration->id);
            if (!daemon->stopping_) daemon->server_.close(expiration->id, 1);
            return G_SOURCE_REMOVE;
        },
        new Expiration{this, id},
        +[](gpointer raw) { delete static_cast<Expiration*>(raw); }
    );
    const guint source_id = g_source_attach(source, context_);
    g_source_unref(source);
    if (source_id == 0) return;
    expiration_sources_[id] = source_id;
}

void NotificationDaemon::cancel_expiration(std::uint32_t id) {
    const auto iterator = expiration_sources_.find(id);
    if (iterator == expiration_sources_.end()) return;
    if (iterator->second != 0) g_source_remove(iterator->second);
    expiration_sources_.erase(iterator);
}

void NotificationDaemon::emit_closed(
    GDBusConnection* connection,
    std::uint32_t id,
    std::uint32_t reason
) {
    if (connection == nullptr || connection != connection_ ||
        registration_id_ == 0 || g_dbus_connection_is_closed(connection)) {
        return;
    }
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
