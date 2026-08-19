#include "services/LockSessionProvider.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

void test_initial_state_unlocked() {
    realmheart::services::LockSessionProvider provider;
    require(!provider.is_locked(), "new provider should not be locked");
    require(provider.outputs().empty(), "new provider should have no outputs");
}

void test_acquire_release_cycle() {
    realmheart::services::LockSessionProvider provider;

    // Without a Wayland display, acquire will fail in a real environment.
    // For CI/testing, we verify the state transitions are correct.
    auto result = provider.acquire();
    // On a headless CI system, this will likely fail to connect to Wayland.
    // That's expected — the test verifies the API contract.
    if (result.acquired) {
        require(provider.is_locked(), "provider must be locked after acquire success");
        provider.release();
        require(!provider.is_locked(), "provider must be unlocked after release");
    } else {
        // Acquire failed (no Wayland display) — verify error message is set.
        require(!result.error.empty(), "failed acquire must have error message");
        require(!provider.is_locked(), "provider must not be locked after failed acquire");
    }
}

void test_unlock_callback_only_when_locked() {
    bool callback_called = false;
    realmheart::services::LockSessionProvider provider;

    provider.set_unlock_callback([&callback_called]() {
        callback_called = true;
    });

    // unlock_session when not locked should NOT call the callback.
    // This is by design for security: the callback must only fire when
    // we are actually releasing a held lock.
    provider.unlock_session();
    require(!callback_called, "unlock callback must NOT be called when not locked");
}

void test_surface_handle_empty_before_acquire() {
    realmheart::services::LockSessionProvider provider;
    // Before acquire, there are no surfaces.
    auto handle = provider.get_surface_handle(0);
    require(handle == 0, "surface handle should be 0 before acquire");
}

void test_display_fd_before_acquire() {
    realmheart::services::LockSessionProvider provider;
    int fd = provider.display_fd();
    // Before acquire, there's no display.
    require(fd == -1, "display_fd should be -1 before acquire");
}

} // namespace

int main() {
    test_initial_state_unlocked();
    test_acquire_release_cycle();
    test_unlock_callback_only_when_locked();
    test_surface_handle_empty_before_acquire();
    test_display_fd_before_acquire();

    std::cout << "All LockSessionProvider tests PASSED\n";
    return 0;
}
