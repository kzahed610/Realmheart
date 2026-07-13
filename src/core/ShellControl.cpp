#include "core/ShellControl.hpp"

#include <gio/gio.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <string>

namespace realmheart::core {
namespace {

constexpr std::string_view kShellApplicationId = "dev.realmheart.shell";

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
    } else {
        g_action_group_activate_action(G_ACTION_GROUP(application), action_name.data(), nullptr);
    }
    if (GDBusConnection* connection = g_application_get_dbus_connection(application)) {
        GCancellable* cancellable = g_cancellable_new();
        {
            FlushDeadline deadline(cancellable);
            GError* flush_error = nullptr;
            static_cast<void>(g_dbus_connection_flush_sync(connection, cancellable, &flush_error));
            g_clear_error(&flush_error);
        }
        g_object_unref(cancellable);
    }
    g_object_unref(application);
    return ShellControlResult::Delivered;
}

} // namespace realmheart::core
