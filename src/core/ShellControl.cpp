#include "core/ShellControl.hpp"

#include <gio/gio.h>

#include <iostream>

namespace realmheart::core {
namespace {

constexpr std::string_view kShellApplicationId = "dev.realmheart.shell";

} // namespace

std::string_view shell_application_id() {
    return kShellApplicationId;
}

ShellControlResult send_shell_command(ShellCommand command) {
    GApplication* application = g_application_new(
        kShellApplicationId.data(),
        G_APPLICATION_DEFAULT_FLAGS
    );

    GError* error = nullptr;
    if (!g_application_register(application, nullptr, &error)) {
        std::cerr << "Unable to register Realmheart shell control client: "
                  << (error != nullptr ? error->message : "unknown error") << '\n';
        g_clear_error(&error);
        g_object_unref(application);
        return ShellControlResult::RegistrationFailed;
    }

    if (!g_application_get_is_remote(application)) {
        g_object_unref(application);
        return ShellControlResult::NotRunning;
    }

    const auto action_name = shell_action_name(command);
    if (action_name.empty()
        || !g_action_group_has_action(G_ACTION_GROUP(application), action_name.data())) {
        g_object_unref(application);
        return ShellControlResult::ActionUnavailable;
    }

    g_action_group_activate_action(G_ACTION_GROUP(application), action_name.data(), nullptr);
    if (GDBusConnection* connection = g_application_get_dbus_connection(application)) {
        g_dbus_connection_flush_sync(connection, nullptr, nullptr);
    }
    g_object_unref(application);
    return ShellControlResult::Delivered;
}

} // namespace realmheart::core
