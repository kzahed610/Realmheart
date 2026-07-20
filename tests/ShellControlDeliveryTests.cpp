#include "core/ShellCommand.hpp"
#include "core/ShellControl.hpp"

#include <gio/gio.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>
#include <poll.h>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct ReceiverContext {
    int write_fd;
    GMainLoop* loop;
};

void receive_wallpaper_path(GSimpleAction*, GVariant* parameter, gpointer user_data) {
    auto* context = static_cast<ReceiverContext*>(user_data);
    const gchar* path = g_variant_get_string(parameter, nullptr);
    const std::string message = std::string(path) + "\n";
    static_cast<void>(::write(context->write_fd, message.data(), message.size()));
    g_main_loop_quit(context->loop);
}

int run_fake_primary(int write_fd) {
    GApplication* application = g_application_new(
        realmheart::core::shell_application_id().data(),
        G_APPLICATION_DEFAULT_FLAGS
    );
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    ReceiverContext context{write_fd, loop};
    GSimpleAction* action = g_simple_action_new("set-wallpaper-path", G_VARIANT_TYPE_STRING);
    g_signal_connect(action, "activate", G_CALLBACK(receive_wallpaper_path), &context);
    g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(action));
    g_object_unref(action);

    GError* error = nullptr;
    if (!g_application_register(application, nullptr, &error) ||
        g_application_get_is_remote(application)) {
        g_clear_error(&error);
        g_main_loop_unref(loop);
        g_object_unref(application);
        return 10;
    }

    const char ready = 'R';
    static_cast<void>(::write(write_fd, &ready, 1));
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    g_object_unref(application);
    ::close(write_fd);
    return 0;
}

bool wait_readable(int fd, int timeout_ms) {
    pollfd descriptor{fd, static_cast<short>(POLLIN | POLLHUP), 0};
    int result = -1;
    do {
        result = ::poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    return result > 0;
}

void terminate_child(pid_t child) {
    static_cast<void>(::kill(child, SIGKILL));
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
}

bool delivers_exact_wallpaper_path() {
    std::array<int, 2> pipe_fds{};
    if (::pipe(pipe_fds.data()) != 0) return false;

    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return false;
    }
    if (child == 0) {
        ::close(pipe_fds[0]);
        _exit(run_fake_primary(pipe_fds[1]));
    }

    ::close(pipe_fds[1]);
    char ready = 0;
    if (!wait_readable(pipe_fds[0], 2000) || ::read(pipe_fds[0], &ready, 1) != 1 || ready != 'R') {
        ::close(pipe_fds[0]);
        terminate_child(child);
        return false;
    }

    constexpr std::string_view expected = "/tmp/realmheart wallpaper fixture.png";
    const auto result = realmheart::core::send_shell_command(
        realmheart::core::ShellCommand::SetWallpaperPath,
        expected
    );

    std::string received;
    std::array<char, 256> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!wait_readable(pipe_fds[0], 100)) continue;
        const ssize_t count = ::read(pipe_fds[0], buffer.data(), buffer.size());
        if (count > 0) received.append(buffer.data(), static_cast<std::size_t>(count));
        else break;
    }
    ::close(pipe_fds[0]);

    int child_status = 0;
    pid_t waited = 0;
    const auto child_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < child_deadline) {
        waited = ::waitpid(child, &child_status, WNOHANG);
        if (waited != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (waited == 0) {
        terminate_child(child);
        return false;
    }
    return result == realmheart::core::ShellControlResult::Delivered
        && waited == child
        && WIFEXITED(child_status)
        && WEXITSTATUS(child_status) == 0
        && received == std::string(expected) + "\n";
}

} // namespace

int main() {
    const auto missing_argument = realmheart::core::send_shell_command(
        realmheart::core::ShellCommand::SetWallpaperPath,
        ""
    );
    if (missing_argument != realmheart::core::ShellControlResult::InvalidArgument) {
        std::cerr << "ShellControlDeliveryTests failed: expected InvalidArgument for an empty wallpaper path\n";
        return 1;
    }

    if (!delivers_exact_wallpaper_path()) {
        std::cerr << "ShellControlDeliveryTests failed: exact wallpaper path was not delivered\n";
        return 1;
    }

    std::cout << "ShellControlDeliveryTests passed\n";
    return 0;
}
