#include "ui/lockscreen/AuthPam.hpp"

#include <glib.h>

#include <sys/socket.h>
#include <sys/stat.h>
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
#include <string_view>
#include <thread>
#include <utility>

#ifndef REALMHEART_AUTH_HELPER_PATH
#define REALMHEART_AUTH_HELPER_PATH ""
#endif
#ifndef REALMHEART_AUTH_HELPER_RELATIVE_PATH
#define REALMHEART_AUTH_HELPER_RELATIVE_PATH ""
#endif

namespace realmheart::ui::lockscreen {
namespace {

// CMake supplies the installed libexec path. The executable-relative fallback
// keeps an uninstalled build diagnosable, but helper_is_secure() still requires
// a root-owned setuid file before it can be executed.
std::string auth_helper_path() {
    const std::string configured = REALMHEART_AUTH_HELPER_PATH;
    if (!configured.empty() && ::access(configured.c_str(), F_OK) == 0) {
        return configured;
    }

    char exe[4096]{};
    const ssize_t len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) return {};
    exe[len] = '\0';

    std::string path(exe);
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    const std::string executable_dir = path.substr(0, slash);
    const std::string relative = REALMHEART_AUTH_HELPER_RELATIVE_PATH;
    if (!relative.empty() && relative.front() != '/') {
        const auto prefix_slash = executable_dir.find_last_of('/');
        if (prefix_slash != std::string::npos) {
            const std::string prefix = executable_dir.substr(0, prefix_slash);
            const std::string derived = prefix + "/" + relative;
            if (::access(derived.c_str(), F_OK) == 0) return derived;
        }
    }
    return executable_dir + "/realmheart-auth-helper";
}

bool helper_is_secure(const std::string& path) noexcept {
    struct stat metadata{};
    if (::stat(path.c_str(), &metadata) != 0 ||
        !S_ISREG(metadata.st_mode) ||
        metadata.st_uid != 0 ||
        (metadata.st_mode & S_ISUID) == 0 ||
        (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return false;
    }
    return ::access(path.c_str(), X_OK) == 0;
}

} // namespace

void SecretBuffer::wipe(char* data, const std::size_t size) noexcept {
    if (data == nullptr) return;
    volatile char* cursor = data;
    for (std::size_t index = 0; index < size; ++index) {
        cursor[index] = '\0';
    }
}

SecretBuffer::SecretBuffer(const std::string_view value) {
    if (value.size() > kMaxBytes) {
        valid_ = false;
        return;
    }
    if (value.empty()) return;
    data_ = std::make_unique<char[]>(value.size());
    std::memcpy(data_.get(), value.data(), value.size());
    size_ = value.size();
}

SecretBuffer::~SecretBuffer() {
    wipe(data_.get(), size_);
}

SecretBuffer::SecretBuffer(SecretBuffer&& other) noexcept
    : data_(std::move(other.data_)),
      size_(std::exchange(other.size_, 0)),
      valid_(std::exchange(other.valid_, true)) {}

SecretBuffer& SecretBuffer::operator=(SecretBuffer&& other) noexcept {
    if (this == &other) return *this;
    wipe(data_.get(), size_);
    data_ = std::move(other.data_);
    size_ = std::exchange(other.size_, 0);
    valid_ = std::exchange(other.valid_, true);
    return *this;
}

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
    cancel();
    state_.reset();
}

void AuthPam::cancel() noexcept {
    if (state_ == nullptr) return;
    state_->cancelled.store(true);
    state_->generation.fetch_add(1);
    const pid_t child_pid = state_->child_pid.load();
    if (child_pid > 0) {
        static_cast<void>(::kill(child_pid, SIGKILL));
    }
    if (worker_.joinable()) worker_.join();
    state_->child_pid.store(0);
    state_->active.store(false);
}

void AuthPam::verify_async(
    std::string username,
    SecretBuffer password,
    ResultCallback callback
) {
    if (state_ == nullptr || !callback || !password.valid() || password.empty()) return;
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
        } else if (!helper_is_secure(helper)) {
            std::cerr << "[Lockscreen] auth: helper missing or insecure: "
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
                    std::size_t remaining = password.size();
                    const char* cursor = password.data();
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
                    const char newline = '\n';
                    if (remaining == 0 && !state->cancelled.load()) {
                        while (::send(socketfd[1], &newline, 1, MSG_NOSIGNAL) < 0 &&
                               errno == EINTR) {
                        }
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

        if (state->cancelled.load()) {
            state->active.store(false);
            return;
        }

        // Deliver on the main thread at HIGH idle priority — the default
        // priority queues behind GTK redraw/resize work, delaying the
        // unlock by seconds on a busy compositor frame.
        auto* context = new ResultContext{
            state, generation, success, std::move(callback)
        };
        const guint source_id = g_idle_add_full(
            G_PRIORITY_HIGH_IDLE,
            +[](gpointer data) -> gboolean {
                auto* ctx = static_cast<ResultContext*>(data);
                const bool current = !ctx->state->cancelled.load() &&
                    ctx->state->generation.load() == ctx->generation;
                ctx->state->active.store(false);
                if (current && ctx->callback) {
                    ctx->callback(ctx->success);
                }
                return G_SOURCE_REMOVE;
            },
            context,
            +[](gpointer data) { delete static_cast<ResultContext*>(data); }
        );
        if (source_id == 0) {
            state->active.store(false);
            delete context;
        }
    });
}

} // namespace realmheart::ui::lockscreen
