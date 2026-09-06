#include "ui/lockscreen/AuthPam.hpp"

#include <glib.h>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <memory>
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
    std::atomic<bool> cancelled{false};
    std::atomic<pid_t> child_pid{0};
    std::atomic<std::uint64_t> generation{0};
};

struct AuthPam::ResultContext {
    std::shared_ptr<AuthPam::State> state;
    std::uint64_t generation = 0;
    bool success = false;
    AuthPam::ResultCallback callback;
};

AuthPam::AuthPam() : state_(std::make_shared<State>()) {}

AuthPam::~AuthPam() {
    if (state_ == nullptr) return;
    state_->cancelled.store(true);
    const pid_t child_pid = state_->child_pid.load();
    if (child_pid > 0) {
        static_cast<void>(::kill(child_pid, SIGKILL));
    }
    if (worker_.joinable()) worker_.join();
    state_.reset();
}

void AuthPam::verify_async(
    std::string username,
    std::string password,
    ResultCallback callback
) {
    if (state_ == nullptr || !callback) return;
    if (state_->active.exchange(true)) return; // one auth at a time

    if (worker_.joinable()) worker_.join();
    state_->cancelled.store(false);
    const std::uint64_t generation = state_->generation.fetch_add(1) + 1;
    const auto state = state_;
    worker_ = std::thread([state, generation, username = std::move(username), password = std::move(password),
                           callback = std::move(callback)]() mutable {
        const std::string helper = auth_helper_path();
        bool success = false;

        if (state->cancelled.load()) {
            state->active.store(false);
            return;
        }

        if (helper.empty()) {
            std::cerr << "[Lockscreen] auth: cannot resolve helper path\n";
        } else if (::access(helper.c_str(), X_OK) != 0) {
            std::cerr << "[Lockscreen] auth: helper missing/not executable: "
                      << helper << "\n";
        } else {
            // Spawn the setuid helper: argv = [helper, username], stdin = password.
            // A socketpair lets the parent use MSG_NOSIGNAL instead of changing
            // the process-wide SIGPIPE disposition.
            int socketfd[2]{};
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socketfd) == 0) {
                const pid_t pid = state->cancelled.load() ? -1 : ::fork();
                if (pid == 0) {
                    // Child: exec the helper.
                    ::dup2(socketfd[0], STDIN_FILENO);
                    ::close(socketfd[0]);
                    ::close(socketfd[1]);
                    ::execl(helper.c_str(), helper.c_str(), username.c_str(),
                            static_cast<char*>(nullptr));
                    _exit(127);
                }
                if (pid < 0) {
                    const int fork_error = errno;
                    ::close(socketfd[0]);
                    ::close(socketfd[1]);
                    std::cerr << "[Lockscreen] auth: fork() failed: "
                              << std::strerror(fork_error) << "\n";
                } else {
                    state->child_pid.store(pid);
                    ::close(socketfd[0]);
                    const std::string payload = password + "\n";
                    std::size_t remaining = payload.size();
                    const char* cursor = payload.data();
                    while (remaining > 0 && !state->cancelled.load()) {
                        const ssize_t written = ::send(
                            socketfd[1], cursor, remaining, MSG_NOSIGNAL
                        );
                        if (written > 0) {
                            cursor += written;
                            remaining -= static_cast<std::size_t>(written);
                            continue;
                        }
                        if (written < 0 && errno == EINTR) continue;
                        break;
                    }
                    ::close(socketfd[1]);

                    int status = 0;
                    bool reaped = false;
                    const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
                    while (!reaped) {
                        if (state->cancelled.load() ||
                            std::chrono::steady_clock::now() >= deadline) {
                            static_cast<void>(::kill(pid, SIGKILL));
                        }
                        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
                        if (waited == pid) {
                            reaped = true;
                            break;
                        }
                        if (waited < 0) {
                            if (errno == EINTR) continue;
                            std::cerr << "[Lockscreen] auth: waitpid() failed: "
                                      << std::strerror(errno) << "\n";
                            break;
                        }
                        if (state->cancelled.load() ||
                            std::chrono::steady_clock::now() >= deadline) {
                            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                            }
                            reaped = true;
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    state->child_pid.store(0);
                    success = reaped && WIFEXITED(status) &&
                        WEXITSTATUS(status) == 0;
                    if (reaped) {
                        std::cerr << "[Lockscreen] auth: helper pid=" << pid
                                  << " exited=" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1)
                                  << " signaled=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0)
                                  << " -> " << (success ? "SUCCESS" : "FAIL") << "\n";
                    }
                }
            } else {
                std::cerr << "[Lockscreen] auth: socketpair() failed: "
                          << std::strerror(errno) << "\n";
            }
        }

        state->active.store(false);
        if (state->cancelled.load()) return;

        // Deliver on the main thread at HIGH idle priority — the default
        // priority queues behind GTK redraw/resize work, delaying the
        // unlock by seconds on a busy compositor frame.
        g_idle_add_full(
            G_PRIORITY_HIGH_IDLE,
            +[](gpointer data) -> gboolean {
                auto* ctx = static_cast<ResultContext*>(data);
                if (!ctx->state->cancelled.load() &&
                    ctx->state->generation.load() == ctx->generation &&
                    ctx->callback) {
                    ctx->callback(ctx->success);
                }
                delete ctx;
                return G_SOURCE_REMOVE;
            },
            new ResultContext{state, generation, success, std::move(callback)},
            nullptr
        );
    });
}

} // namespace realmheart::ui::lockscreen
