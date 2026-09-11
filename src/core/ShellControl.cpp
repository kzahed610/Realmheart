#include "core/ShellControl.hpp"

#include <gio/gio.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <string>
#include <unistd.h>

namespace realmheart::core {
namespace {

constexpr std::string_view kShellApplicationId = "dev.realmheart.shell";
constexpr std::string_view kLockControlObjectPath = "/dev/realmheart/ShellControl";
constexpr std::string_view kLockControlInterface = "dev.realmheart.ShellControl";

class FlushDeadline {
public:
    explicit FlushDeadline(GCancellable* cancellable)
        : cancellable_(cancellable), thread_([this] {
            std::unique_lock lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::seconds(1), [this] { return done_; })) {
                g_cancellable_cancel(cancellable_);
            }
        }) {}
    ~FlushDeadline() {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
private:
    GCancellable* cancellable_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread thread_;
};

} // namespace

std::string_view shell_application_id() {
    return kShellApplicationId;
}

ShellControlResult request_shell_lock(std::string_view request_token) {
    std::string token(request_token);
    if (token.empty()) {
        token = std::to_string(static_cast<long long>(::getpid())) + "-" +
            std::to_string(static_cast<long long>(g_get_monotonic_time()));
    }

    GApplication* application = g_application_new(
        kShellApplicationId.data(),
        G_APPLICATION_DEFAULT_FLAGS
    );
    GError* error = nullptr;
    if (!g_application_register(application, nullptr, &error)) {
        g_clear_error(&error);
        g_object_unref(application);
        return ShellControlResult::RegistrationFailed;
    }
    if (!g_application_get_is_remote(application)) {
        g_object_unref(application);
        return ShellControlResult::NotRunning;
    }

    GDBusConnection* connection = g_application_get_dbus_connection(application);
    if (connection == nullptr) {
        g_object_unref(application);
        return ShellControlResult::DeliveryFailed;
    }

    GError* call_error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection,
        kShellApplicationId.data(),
        kLockControlObjectPath.data(),
        kLockControlInterface.data(),
        "LockSession",
        g_variant_new("(s)", token.c_str()),
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        6000,
        nullptr,
        &call_error
    );
    if (reply == nullptr) {
        g_clear_error(&call_error);
        g_object_unref(application);
        return ShellControlResult::LockFailed;
    }

    const char* status = nullptr;
    g_variant_get(reply, "(&s)", &status);
    const bool ready = status != nullptr && std::string_view(status) == "native";
    g_variant_unref(reply);
    g_object_unref(application);
    return ready ? ShellControlResult::LockReady : ShellControlResult::LockFailed;
}

ShellControlResult send_shell_command(ShellCommand command, std::string_view argument) {
    const bool requires_argument = shell_command_requires_argument(command);
    if (requires_argument && argument.empty()) {
        return ShellControlResult::InvalidArgument;
    }

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

    if (requires_argument) {
        const std::string owned_argument(argument);
        g_action_group_activate_action(
            G_ACTION_GROUP(application),
            action_name.data(),
            g_variant_new_string(owned_argument.c_str())
        );
    } else if (command == ShellCommand::LockSession) {
        // Tokenized lock requests use request_shell_lock(), which waits for a
        // private D-Bus acknowledgement. This branch remains the deliberate
        // fire-and-forget path used by SUPER+L.
        const std::string owned_argument(argument);
        g_action_group_activate_action(
            G_ACTION_GROUP(application),
            action_name.data(),
            g_variant_new_string(owned_argument.c_str())
        );
    } else {
        g_action_group_activate_action(G_ACTION_GROUP(application), action_name.data(), nullptr);
    }
    GDBusConnection* connection = g_application_get_dbus_connection(application);
    if (connection == nullptr) {
        g_object_unref(application);
        return ShellControlResult::DeliveryFailed;
    }

    GCancellable* cancellable = g_cancellable_new();
    bool flushed = false;
    {
        FlushDeadline deadline(cancellable);
        GError* flush_error = nullptr;
        flushed = g_dbus_connection_flush_sync(connection, cancellable, &flush_error);
        if (!flushed && flush_error != nullptr) {
            std::cerr << "Unable to flush Realmheart shell command: "
                      << flush_error->message << '\n';
        }
        g_clear_error(&flush_error);
    }
    g_object_unref(cancellable);
    g_object_unref(application);
    return flushed ? ShellControlResult::Delivered : ShellControlResult::DeliveryFailed;
}

} // namespace realmheart::core
