#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace realmheart::ui::lockscreen {

// Asynchronous PAM authentication for the lockscreen.
// Spawns the setuid-root realmheart-auth-helper (which can read /etc/shadow)
// on a worker thread and invokes the callback on the main (GTK) thread with
// the result. Keeps the render loop unblocked.
class AuthPam {
public:
    using ResultCallback = std::function<void(bool success)>;

    AuthPam();
    ~AuthPam();

    AuthPam(const AuthPam&) = delete;
    AuthPam& operator=(const AuthPam&) = delete;

    // Verifies the password for the given username asynchronously.
    // The callback is invoked exactly once on the main thread.
    void verify_async(
        std::string username,
        std::string password,
        ResultCallback callback
    );

private:
    struct State;
    struct ResultContext;
    std::shared_ptr<State> state_;
    std::thread worker_;
};

} // namespace realmheart::ui::lockscreen
