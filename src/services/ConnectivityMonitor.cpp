#include "services/ConnectivityMonitor.hpp"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace realmheart::services {

struct ConnectivityMonitorState {
    ConnectivityMonitor::ChangedCallback changed;
    GDBusConnection* supplied_connection = nullptr;
    GDBusConnection* connection = nullptr;
    GMainContext* context = nullptr;
    std::vector<guint> subscriptions;
    gulong closed_handler = 0;
    guint debounce_id = 0;
    guint reconnect_id = 0;
    guint reconnect_attempts = 0;
    bool running = false;

    ~ConnectivityMonitorState() {
        g_clear_object(&connection);
        g_clear_object(&supplied_connection);
        if (context != nullptr) g_main_context_unref(context);
    }
};

namespace {

using State = ConnectivityMonitorState;
using StateRef = std::shared_ptr<State>;

constexpr guint kChangeDebounceMs = 80;
constexpr std::size_t kExpectedSubscriptions = 9;

gpointer hold_state(const StateRef& state) {
    return new StateRef(state);
}

StateRef state_from(gpointer raw) {
    return *static_cast<StateRef*>(raw);
}

void destroy_state(gpointer raw) {
    delete static_cast<StateRef*>(raw);
}

void destroy_closure_state(gpointer raw, GClosure*) {
    destroy_state(raw);
}

void destroy_source(const StateRef& state, guint& source_id) {
    if (source_id == 0) return;
    if (state->context != nullptr) {
        if (GSource* source = g_main_context_find_source_by_id(
                state->context, source_id
            )) {
            g_source_destroy(source);
        }
    }
    source_id = 0;
}

guint attach_timeout(
    const StateRef& state,
    guint milliseconds,
    GSourceFunc callback
) {
    GSource* source = g_timeout_source_new(milliseconds);
    g_source_set_priority(source, G_PRIORITY_DEFAULT);
    g_source_set_callback(source, callback, hold_state(state), destroy_state);
    const guint source_id = g_source_attach(source, state->context);
    g_source_unref(source);
    return source_id;
}

void schedule_notification(const StateRef& state);
bool connect_to_bus(const StateRef& state);

gboolean on_notification_timeout(gpointer raw) {
    const auto state = state_from(raw);
    state->debounce_id = 0;
    if (state->running && state->changed) state->changed();
    return G_SOURCE_REMOVE;
}

void schedule_notification(const StateRef& state) {
    if (!state->running || state->context == nullptr) return;
    if (state->debounce_id != 0) return;
    state->debounce_id = attach_timeout(
        state,
        kChangeDebounceMs,
        &on_notification_timeout
    );
}

void reset_connection(const StateRef& state, bool disconnect_closed_handler = true) {
    if (state->connection == nullptr) return;
    for (const guint subscription : state->subscriptions) {
        g_dbus_connection_signal_unsubscribe(state->connection, subscription);
    }
    state->subscriptions.clear();
    if (disconnect_closed_handler && state->closed_handler != 0 &&
        g_signal_handler_is_connected(state->connection, state->closed_handler)) {
        g_signal_handler_disconnect(state->connection, state->closed_handler);
    }
    state->closed_handler = 0;
    g_clear_object(&state->connection);
}

void schedule_reconnect(const StateRef& state);

void on_connection_closed(
    GDBusConnection* connection,
    gboolean,
    GError*,
    gpointer raw
) {
    const auto state = state_from(raw);
    if (state->connection != connection) return;
    const gulong handler = state->closed_handler;
    state->closed_handler = 0;
    if (handler != 0 && g_signal_handler_is_connected(connection, handler)) {
        g_signal_handler_disconnect(connection, handler);
    }
    reset_connection(state, false);
    schedule_notification(state);
    schedule_reconnect(state);
}

gboolean on_reconnect_timeout(gpointer raw) {
    const auto state = state_from(raw);
    state->reconnect_id = 0;
    if (state->running) static_cast<void>(connect_to_bus(state));
    return G_SOURCE_REMOVE;
}

void schedule_reconnect(const StateRef& state) {
    if (!state->running || state->supplied_connection != nullptr ||
        state->context == nullptr || state->reconnect_id != 0) {
        return;
    }
    const guint shift = std::min(state->reconnect_attempts, 5U);
    const guint delay = std::min(10'000U, 250U << shift);
    ++state->reconnect_attempts;
    state->reconnect_id = attach_timeout(
        state,
        delay,
        &on_reconnect_timeout
    );
}

bool subscribe_signal(
    const StateRef& state,
    const char* sender,
    const char* interface_name,
    const char* member,
    const char* object_path,
    GDBusSignalCallback callback
) {
    gpointer user_data = hold_state(state);
    const guint subscription = g_dbus_connection_signal_subscribe(
        state->connection,
        sender,
        interface_name,
        member,
        object_path,
        nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        callback,
        user_data,
        &destroy_state
    );
    if (subscription == 0) {
        destroy_state(user_data);
        return false;
    }
    state->subscriptions.push_back(subscription);
    return true;
}

bool is_network_manager_properties_interface(const char* interface_name) {
    if (interface_name == nullptr) return false;
    const std::string_view name(interface_name);
    return name == "org.freedesktop.NetworkManager" ||
        name.starts_with("org.freedesktop.NetworkManager.");
}

bool is_relevant_bluez_property(
    std::string_view interface_name,
    std::string_view property_name
) {
    if (interface_name == "org.bluez.Adapter1") {
        return property_name == "Powered";
    }
    if (interface_name != "org.bluez.Device1") return false;
    return property_name == "Connected" || property_name == "Paired" ||
        property_name == "Name" || property_name == "Alias";
}

bool has_relevant_bluez_changed_property(
    const char* interface_name,
    GVariant* changed_properties
) {
    if (interface_name == nullptr) return false;
    GVariantIter iterator;
    g_variant_iter_init(&iterator, changed_properties);
    const gchar* property_name = nullptr;
    GVariant* value = nullptr;
    while (g_variant_iter_next(&iterator, "{&sv}", &property_name, &value)) {
        const bool relevant = is_relevant_bluez_property(
            interface_name, property_name
        );
        g_variant_unref(value);
        if (relevant) return true;
    }
    return false;
}

bool has_relevant_bluez_invalidated_property(
    const char* interface_name,
    GVariant* invalidated_properties
) {
    if (interface_name == nullptr) return false;
    GVariantIter iterator;
    g_variant_iter_init(&iterator, invalidated_properties);
    const gchar* property_name = nullptr;
    while (g_variant_iter_next(&iterator, "&s", &property_name)) {
        if (is_relevant_bluez_property(interface_name, property_name)) return true;
    }
    return false;
}

void on_properties_changed(
    GDBusConnection*,
    const gchar*,
    const gchar*,
    const gchar*,
    const gchar*,
    GVariant* parameters,
    gpointer raw
) {
    const auto state = state_from(raw);
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sa{sv}as)"))) return;
    const gchar* changed_interface = nullptr;
    GVariant* changed_properties = nullptr;
    GVariant* invalidated_properties = nullptr;
    g_variant_get(
        parameters,
        "(&s@a{sv}@as)",
        &changed_interface,
        &changed_properties,
        &invalidated_properties
    );
    const bool relevant =
        is_network_manager_properties_interface(changed_interface) ||
        has_relevant_bluez_changed_property(
            changed_interface, changed_properties
        ) ||
        has_relevant_bluez_invalidated_property(
            changed_interface, invalidated_properties
        );
    g_variant_unref(changed_properties);
    g_variant_unref(invalidated_properties);
    if (relevant) schedule_notification(state);
}

void on_owner_changed(
    GDBusConnection*,
    const gchar*,
    const gchar*,
    const gchar*,
    const gchar*,
    GVariant* parameters,
    gpointer raw
) {
    const auto state = state_from(raw);
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)"))) return;
    const gchar* name = nullptr;
    const gchar* old_owner = nullptr;
    const gchar* new_owner = nullptr;
    g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
    (void)old_owner;
    (void)new_owner;
    if (g_str_equal(name, "org.freedesktop.NetworkManager") ||
        g_str_equal(name, "org.bluez")) {
        schedule_notification(state);
    }
}

void on_connectivity_signal(
    GDBusConnection*,
    const gchar*,
    const gchar*,
    const gchar*,
    const gchar*,
    GVariant*,
    gpointer raw
) {
    schedule_notification(state_from(raw));
}

bool connect_to_bus(const StateRef& state) {
    if (!state->running) return false;
    if (state->connection != nullptr &&
        !g_dbus_connection_is_closed(state->connection) &&
        state->subscriptions.size() == kExpectedSubscriptions) {
        return true;
    }
    reset_connection(state);

    GError* error = nullptr;
    GDBusConnection* connection = nullptr;
    if (state->supplied_connection != nullptr) {
        if (!g_dbus_connection_is_closed(state->supplied_connection)) {
            connection = G_DBUS_CONNECTION(g_object_ref(state->supplied_connection));
        }
    } else {
        connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    }
    g_clear_error(&error);
    if (connection == nullptr || g_dbus_connection_is_closed(connection)) {
        if (connection != nullptr) g_object_unref(connection);
        schedule_reconnect(state);
        return false;
    }

    state->connection = connection;
    gpointer closed_handler_data = hold_state(state);
    state->closed_handler = g_signal_connect_data(
        state->connection,
        "closed",
        G_CALLBACK(&on_connection_closed),
        closed_handler_data,
        &destroy_closure_state,
        G_CONNECT_DEFAULT
    );
    if (state->closed_handler == 0) destroy_state(closed_handler_data);

    const bool subscribed =
        subscribe_signal(
            state,
            "org.freedesktop.NetworkManager",
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            nullptr,
            &on_properties_changed
        ) &&
        subscribe_signal(
            state,
            "org.bluez",
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            nullptr,
            &on_properties_changed
        ) &&
        subscribe_signal(
            state,
            "org.freedesktop.DBus",
            "org.freedesktop.DBus",
            "NameOwnerChanged",
            "/org/freedesktop/DBus",
            &on_owner_changed
        ) &&
        subscribe_signal(
            state,
            "org.bluez",
            "org.freedesktop.DBus.ObjectManager",
            "InterfacesAdded",
            "/",
            &on_connectivity_signal
        ) &&
        subscribe_signal(
            state,
            "org.bluez",
            "org.freedesktop.DBus.ObjectManager",
            "InterfacesRemoved",
            "/",
            &on_connectivity_signal
        ) &&
        subscribe_signal(
            state,
            "org.freedesktop.NetworkManager",
            "org.freedesktop.NetworkManager",
            "DeviceAdded",
            "/org/freedesktop/NetworkManager",
            &on_connectivity_signal
        ) &&
        subscribe_signal(
            state,
            "org.freedesktop.NetworkManager",
            "org.freedesktop.NetworkManager",
            "DeviceRemoved",
            "/org/freedesktop/NetworkManager",
            &on_connectivity_signal
        ) &&
        subscribe_signal(
            state,
            "org.freedesktop.NetworkManager",
            "org.freedesktop.NetworkManager.Device.Wireless",
            "AccessPointAdded",
            nullptr,
            &on_connectivity_signal
        ) &&
        subscribe_signal(
            state,
            "org.freedesktop.NetworkManager",
            "org.freedesktop.NetworkManager.Device.Wireless",
            "AccessPointRemoved",
            nullptr,
            &on_connectivity_signal
        );

    if (!subscribed) {
        reset_connection(state);
        schedule_reconnect(state);
        return false;
    }
    state->reconnect_attempts = 0;
    return true;
}

} // namespace

ConnectivityMonitor::ConnectivityMonitor(
    ChangedCallback changed,
    GDBusConnection* injected_connection
) : state_(std::make_shared<ConnectivityMonitorState>()) {
    state_->changed = std::move(changed);
    if (injected_connection != nullptr) {
        state_->supplied_connection = G_DBUS_CONNECTION(g_object_ref(injected_connection));
    }
}

ConnectivityMonitor::~ConnectivityMonitor() {
    stop();
}

void ConnectivityMonitor::start() {
    if (state_->running) return;
    state_->running = true;
    if (state_->context == nullptr) {
        state_->context = g_main_context_ref_thread_default();
    }
    static_cast<void>(connect_to_bus(state_));
}

void ConnectivityMonitor::stop() {
    state_->running = false;
    destroy_source(state_, state_->debounce_id);
    destroy_source(state_, state_->reconnect_id);
    reset_connection(state_);
}

bool ConnectivityMonitor::signal_monitor_active() const noexcept {
    return state_->running && state_->connection != nullptr &&
        !g_dbus_connection_is_closed(state_->connection) &&
        state_->subscriptions.size() == kExpectedSubscriptions;
}

} // namespace realmheart::services
