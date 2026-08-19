#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace realmheart::services {

// LockSessionProvider tracks the lock state for the parent process.
//
// The actual ext-session-lock-v1 protocol is handled entirely by the
// renderer subprocess (realmheart-lockscreen-renderer). This class
// simply tracks whether we consider the session locked and provides
// a callback when unlock is requested.
//
// Security model:
//   - Renderer acquires ext-session-lock-v1 and renders lock surfaces
//   - Renderer runs PAM authentication
//   - On auth success, renderer sends "UNLOCK" over control socket
//   - Parent calls ext_session_lock_v1_destroy() to release the lock
//   - The renderer can never directly release the lock
class LockSessionProvider {
public:
    using UnlockCallback = std::function<void()>;

    LockSessionProvider() = default;
    ~LockSessionProvider() = default;

    LockSessionProvider(const LockSessionProvider&) = delete;
    LockSessionProvider& operator=(const LockSessionProvider&) = delete;

    // Mark the session as locked (called after renderer reports LOCKED).
    void mark_locked() { locked_ = true; }

    // Release the session lock. Calls the unlock callback if registered.
    void unlock_session();

    // Check if currently holding the session lock.
    bool is_locked() const { return locked_; }

    // Register callback for when the renderer requests unlock.
    void set_unlock_callback(UnlockCallback callback) {
        unlock_callback_ = std::move(callback);
    }

private:
    UnlockCallback unlock_callback_;
    bool locked_ = false;
};

} // namespace realmheart::services
