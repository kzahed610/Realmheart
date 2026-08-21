#include "ui/lockscreen/AuthPam.hpp"

#include <glib.h>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace realmheart::ui::lockscreen {
namespace {

// Resolves the auth helper next to the running executable.
std::string auth_helper_path() {
    char exe[4096]{};
    const ssize_t len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) return {};
    exe[len] = '\0';

    std::string path(exe);
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return path.substr(0, slash + 1) + "realmheart-auth-helper";
}

} // namespace

struct AuthPam::State {
    std::atomic<bool> active{false};
};

AuthPam::AuthPam() : state_(new State) {}

AuthPam::~AuthPam() {
    delete state_;
    state_ = nullptr;
}

void AuthPam::verify_async(
    std::string username,
    std::string password,
    ResultCallback callback
) {
    if (state_ == nullptr || !callback) return;
    if (state_->active.exchange(true)) return; // one auth at a time

    std::thread([this, username = std::move(username), password = std::move(password),
                 callback = std::move(callback)]() mutable {
        const std::string helper = auth_helper_path();
        bool success = false;

        if (!helper.empty()) {
            // Spawn the setuid helper: argv = [helper, username], stdin = password.
            int pipefd[2]{};
            if (::pipe(pipefd) == 0) {
                const pid_t pid = ::fork();
                if (pid == 0) {
                    // Child: exec the helper.
                    ::dup2(pipefd[0], STDIN_FILENO);
                    ::close(pipefd[0]);
                    ::close(pipefd[1]);
                    ::execl(helper.c_str(), helper.c_str(), username.c_str(),
                            static_cast<char*>(nullptr));
                    _exit(127);
                }
                // Parent: write the password, close, reap.
                ::close(pipefd[0]);
                const std::string payload = password + "\n";
                ssize_t written = 0;
                while (written < static_cast<ssize_t>(payload.size())) {
                    const ssize_t n = ::write(
                        pipefd[1],
                        payload.data() + written,
                        static_cast<std::size_t>(payload.size() - written)
                    );
                    if (n <= 0) break;
                    written += n;
                }
                ::close(pipefd[1]);

                int status = 0;
                ::waitpid(pid, &status, 0);
                success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
        }

        // Deliver on the main thread.
        g_idle_add(+[](gpointer data) -> gboolean {
            auto* ctx = static_cast<std::pair<bool, ResultCallback>*>(data);
            if (ctx->second) ctx->second(ctx->first);
            delete ctx;
            return G_SOURCE_REMOVE;
        }, new std::pair<bool, ResultCallback>(success, std::move(callback)));

        state_->active.store(false);
    }).detach();
}

} // namespace realmheart::ui::lockscreen
