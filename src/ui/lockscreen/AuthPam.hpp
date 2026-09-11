#pragma once

#include <functional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace realmheart::ui::lockscreen {

// Returns true only for the root-owned, setuid, non-writable executable that
// the PAM boundary requires. Kept as a narrow contract so deployment tests can
// exercise the same predicate used by AuthPam without invoking PAM.
[[nodiscard]] bool auth_helper_is_secure(const std::string& path) noexcept;

// Bounded, move-only storage for password material. The allocation is wiped
// before release so authentication code never needs to copy a std::string
// containing a password through a worker or IPC payload.
class SecretBuffer {
public:
    static constexpr std::size_t kMaxBytes = 512;

    SecretBuffer() = default;
    explicit SecretBuffer(std::string_view value);
    ~SecretBuffer();

    SecretBuffer(const SecretBuffer&) = delete;
    SecretBuffer& operator=(const SecretBuffer&) = delete;
    SecretBuffer(SecretBuffer&& other) noexcept;
    SecretBuffer& operator=(SecretBuffer&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const char* data() const noexcept { return data_.get(); }

private:
    static void wipe(char* data, std::size_t size) noexcept;

    std::unique_ptr<char[]> data_;
    std::size_t size_ = 0;
    bool valid_ = true;
};

// Asynchronous PAM authentication for the lockscreen.
// Spawns the setuid-root realmheart-auth-helper (which can read /etc/shadow)
// on a worker thread and invokes the callback on the main (GTK) thread with
// the result. Keeps the render loop unblocked.
class AuthPam {
public:
    static constexpr std::size_t kMaxPasswordBytes = SecretBuffer::kMaxBytes;
    using ResultCallback = std::function<void(bool success)>;

    AuthPam();
    ~AuthPam();

    AuthPam(const AuthPam&) = delete;
    AuthPam& operator=(const AuthPam&) = delete;

    // Cancels the current helper and waits for its worker to finish. This is
    // intentionally synchronous so a surface can be destroyed without a
    // child process or callback retaining credential material.
    void cancel() noexcept;

    // Verifies the password for the given username asynchronously.
    // The callback is invoked exactly once on the main thread.
    void verify_async(
        std::string username,
        SecretBuffer password,
        ResultCallback callback
    );

private:
    struct State;
    struct ResultContext;
    std::shared_ptr<State> state_;
    std::thread worker_;
};

} // namespace realmheart::ui::lockscreen
