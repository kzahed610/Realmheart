#include "core/ShellCommand.hpp"
#include "core/ShellControl.hpp"

#include <gio/gio.h>

#include <array>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct ReceiverContext {
    int write_fd;
    GMainLoop* loop;
};

void receive_wallpaper_path(
    GSimpleAction* /*action*/,
    GVariant* parameter,
    gpointer user_data
) {
    auto* context = static_cast<ReceiverContext*>(user_data);
    const gchar* path = g_variant_get_string(parameter, nullptr);
    const std::string message = std::string(path) + "\n";
    (void)write(context->write_fd, message.data(), message.size());
    g_main_loop_quit(context->loop);
}

int run_fake_primary(int write_fd) {
    GApplication* application = g_application_new(
        realmheart::core::shell_application_id().data(),
        G_APPLICATION_DEFAULT_FLAGS
    );

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    ReceiverContext context{write_fd, loop};
    GSimpleAction* action = g_simple_action_new(
        "set-wallpaper-path",
        G_VARIANT_TYPE_STRING
    );
    g_signal_connect(action, "activate", G_CALLBACK(receive_wallpaper_path), &context);
    g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(action));
    g_object_unref(action);

    GError* error = nullptr;
    if (!g_application_register(application, nullptr, &error)) {
        g_clear_error(&error);
        g_main_loop_unref(loop);
        g_object_unref(application);
        return 10;
    }

    const char ready = 'R';
    (void)write(write_fd, &ready, 1);
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    g_object_unref(application);
    close(write_fd);
    return 0;
}

bool delivers_exact_wallpaper_path() {
    std::array<int, 2> pipe_fds{};
    if (pipe(pipe_fds.data()) != 0) return false;

    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        close(pipe_fds[0]);
        _exit(run_fake_primary(pipe_fds[1]));
    }

    close(pipe_fds[1]);
    char ready = 0;
    if (read(pipe_fds[0], &ready, 1) != 1 || ready != 'R') return false;

    constexpr std::string_view expected = "/tmp/realmheart wallpaper fixture.png";
    const auto result = realmheart::core::send_shell_command(
        realmheart::core::ShellCommand::SetWallpaperPath,
        expected
    );

    std::string received;
    std::array<char, 256> buffer{};
    ssize_t count = 0;
    while ((count = read(pipe_fds[0], buffer.data(), buffer.size())) > 0) {
        received.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(pipe_fds[0]);

    int child_status = 0;
    (void)waitpid(child, &child_status, 0);
    return result == realmheart::core::ShellControlResult::Delivered
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
        std::cerr << "ShellRuntimeTests failed: expected InvalidArgument for an empty wallpaper path\n";
        return 1;
    }

    if (!delivers_exact_wallpaper_path()) {
        std::cerr << "ShellRuntimeTests failed: exact wallpaper path was not delivered\n";
        return 1;
    }

    std::cout << "ShellRuntimeTests passed\n";
    return 0;
}
