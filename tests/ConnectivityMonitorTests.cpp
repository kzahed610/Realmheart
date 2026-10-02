#include "services/ConnectivityMonitor.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

GDBusConnection* session_connection() {
    GError* error = nullptr;
    auto* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection == nullptr) {
        const std::string detail = error != nullptr ? error->message : "unknown error";
        g_clear_error(&error);
        throw std::runtime_error("session bus connection failed: " + detail);
    }
    return connection;
}

void bus_name_call(GDBusConnection* connection, const char* method, const char* name) {
    GError* error = nullptr;
    GVariant* arguments = std::string_view(method) == "RequestName"
        ? g_variant_new("(su)", name, 0U)
        : g_variant_new("(s)", name);
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        method,
        arguments,
        G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        1500,
        nullptr,
        &error
    );
    if (reply == nullptr) {
        const std::string detail = error != nullptr ? error->message : "unknown error";
        g_clear_error(&error);
        throw std::runtime_error(std::string(method) + " failed: " + detail);
    }
    guint32 result = 0;
    g_variant_get(reply, "(u)", &result);
    g_variant_unref(reply);
    if (std::string_view(method) == "RequestName") {
        require(result == 1U, "test service name must become the primary owner");
    }
}

void emit_signal(
    GDBusConnection* connection,
    const char* path,
    const char* interface_name,
    const char* member,
    GVariant* parameters
) {
    GError* error = nullptr;
    const gboolean emitted = g_dbus_connection_emit_signal(
        connection,
        nullptr,
        path,
        interface_name,
        member,
        parameters,
        &error
    );
    if (!emitted) {
        const std::string detail = error != nullptr ? error->message : "unknown error";
        g_clear_error(&error);
        throw std::runtime_error("test signal emit failed: " + detail);
    }
    if (!g_dbus_connection_flush_sync(connection, nullptr, &error)) {
        const std::string detail = error != nullptr ? error->message : "unknown error";
        g_clear_error(&error);
        throw std::runtime_error("test signal flush failed: " + detail);
    }
}

void emit_properties_changed(
    GDBusConnection* connection,
    const char* path,
    const char* changed_interface,
    const char* changed_property = "State"
) {
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    const std::string_view property_name(changed_property);
    GVariant* property_value = property_name == "Connected" ||
            property_name == "Paired"
        ? g_variant_new_boolean(TRUE)
        : (property_name == "RSSI"
            ? g_variant_new_int16(-42)
            : g_variant_new_uint32(100U));
    g_variant_builder_add(&changed, "{sv}", changed_property, property_value);
    emit_signal(
        connection,
        path,
        "org.freedesktop.DBus.Properties",
        "PropertiesChanged",
        g_variant_new(
            "(s@a{sv}@as)",
            changed_interface,
            g_variant_builder_end(&changed),
            g_variant_new_strv(nullptr, 0)
        )
    );
}

void dispatch_for(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    do {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
}

void test_network_and_bluetooth_changes_are_reported() {
    GDBusConnection* connection = session_connection();
    bus_name_call(connection, "RequestName", "org.freedesktop.NetworkManager");
    bus_name_call(connection, "RequestName", "org.bluez");

    int change_count = 0;
    realmheart::services::ConnectivityMonitor monitor(
        [&] { ++change_count; },
        connection
    );
    monitor.start();
    require(monitor.signal_monitor_active(), "system-state signal subscriptions must be active");

    emit_properties_changed(
        connection,
        "/org/freedesktop/NetworkManager/Devices/1",
        "org.freedesktop.NetworkManager.Device.Wireless"
    );
    dispatch_for(180ms);
    require(change_count == 1, "NetworkManager property changes must refresh connectivity");

    emit_properties_changed(
        connection,
        "/org/bluez/hci0/dev_01_02_03_04_05_06",
        "org.bluez.Device1",
        "Connected"
    );
    dispatch_for(180ms);
    require(change_count == 2, "BlueZ device property changes must refresh connectivity");

    emit_properties_changed(
        connection,
        "/org/bluez/hci0/dev_01_02_03_04_05_06",
        "org.bluez.Device1",
        "RSSI"
    );
    dispatch_for(180ms);
    require(change_count == 2, "irrelevant BlueZ signal-strength changes must be ignored");

    emit_properties_changed(
        connection,
        "/org/freedesktop/NetworkManager/Devices/1",
        "org.example.Unrelated"
    );
    dispatch_for(180ms);
    require(change_count == 2, "unrelated property interfaces must be ignored");

    emit_signal(
        connection,
        "/org/freedesktop/NetworkManager/Devices/1",
        "org.freedesktop.NetworkManager.Device.Wireless",
        "AccessPointAdded",
        g_variant_new("(o)", "/org/freedesktop/NetworkManager/AccessPoints/1")
    );
    dispatch_for(180ms);
    require(change_count == 3, "wireless access-point changes must refresh an open network list");

    bus_name_call(connection, "ReleaseName", "org.freedesktop.NetworkManager");
    dispatch_for(180ms);
    require(change_count == 4, "NetworkManager owner changes must refresh connectivity");

    monitor.stop();
    require(!monitor.signal_monitor_active(), "stopped monitor must release its subscriptions");
    emit_properties_changed(
        connection,
        "/org/bluez/hci0/dev_01_02_03_04_05_06",
        "org.bluez.Device1"
    );
    dispatch_for(180ms);
    require(change_count == 4, "stopped monitor must not deliver callbacks");

    bus_name_call(connection, "ReleaseName", "org.bluez");
    g_object_unref(connection);
}

} // namespace

int main() {
    try {
        test_network_and_bluetooth_changes_are_reported();
        std::cout << "ConnectivityMonitorTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ConnectivityMonitorTests failed: " << error.what() << '\n';
        return 1;
    }
}
