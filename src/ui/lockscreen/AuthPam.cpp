#include "ui/lockscreen/AuthPam.hpp"

#include <glib.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
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

// Resolve the helper next to the installed executable first. This keeps
// `cmake --install --prefix ...` relocatable at runtime; the configured absolute
// path remains a compatibility fallback for older system installations. Every
// candidate still passes helper_is_secure() before it can be executed.
std::string auth_helper_path() {
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

    const std::string configured = REALMHEART_AUTH_HELPER_PATH;
    if (!configured.empty() && ::access(configured.c_str(), F_OK) == 0) {
        return configured;
    }
    return executable_dir + "/realmheart-auth-helper";
}

bool secure_directory(const struct stat& metadata) noexcept {
    return S_ISDIR(metadata.st_mode) &&
        metadata.st_uid == 0 &&
        (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

int open_secure_helper(const std::string& path) noexcept {
    if (path.empty() || path.front() != '/') return -1;

    int directory = ::open(
        "/",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
    );
    if (directory < 0) return -1;

    std::size_t cursor = 1;
    while (cursor < path.size()) {
        const std::size_t slash = path.find('/', cursor);
        const bool final_component = slash == std::string::npos;
        const std::size_t length = (final_component ? path.size() : slash) - cursor;
        if (length == 0) {
            cursor = slash + 1;
            continue;
        }
        const std::string component = path.substr(cursor, length);
        if (component == "." || component == "..") {
            ::close(directory);
            return -1;
        }

        if (final_component) {
            const int helper = ::openat(
                directory,
                component.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW
            );
            ::close(directory);
            if (helper < 0) return -1;

            struct stat metadata{};
            if (::fstat(helper, &metadata) != 0 ||
                !S_ISREG(metadata.st_mode) ||
                metadata.st_uid != 0 ||
                (metadata.st_mode & S_ISUID) == 0 ||
                (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
                (metadata.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
                ::close(helper);
                return -1;
            }
            return helper;
        }

        const int next_directory = ::openat(
            directory,
            component.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
        );
        ::close(directory);
        if (next_directory < 0) return -1;
        struct stat metadata{};
        if (::fstat(next_directory, &metadata) != 0 ||
            !secure_directory(metadata)) {
            ::close(next_directory);
            return -1;
        }
        directory = next_directory;
        cursor = slash + 1;
    }
    ::close(directory);
    return -1;
}

bool helper_is_secure(const std::string& path) noexcept {
    const int helper = open_secure_helper(path);
    if (helper < 0) return false;
    ::close(helper);
    return true;
}

} // namespace

bool auth_helper_is_secure(const std::string& path) noexcept {
    return helper_is_secure(path);
}

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
        } else {
            const int helper_fd = open_secure_helper(helper);
            if (helper_fd < 0) {
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
                    char* helper_argv[] = {
                        const_cast<char*>(helper.c_str()),
                        const_cast<char*>(username.c_str()),
                        nullptr
                    };
                    ::fexecve(helper_fd, helper_argv, environ);
                    _exit(127);
                }
                if (pid < 0) {
                    const int fork_error = errno;
                    ::close(helper_fd);
                    ::close(socketfd[0]);
                    ::close(socketfd[1]);
                    std::cerr << "[Lockscreen] auth: fork() failed: "
                              << std::strerror(fork_error) << "\n";
                } else {
                    ::close(helper_fd);
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
                ::close(helper_fd);
                std::cerr << "[Lockscreen] auth: socketpair() failed: "
                          << std::strerror(errno) << "\n";
            }
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
