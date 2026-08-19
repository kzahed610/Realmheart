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
}

void test_mark_locked_and_unlock() {
    realmheart::services::LockSessionProvider provider;

    provider.mark_locked();
    require(provider.is_locked(), "provider must be locked after mark_locked");

    provider.unlock_session();
    require(!provider.is_locked(), "provider must be unlocked after unlock_session");
}

void test_unlock_callback_only_when_locked() {
    bool callback_called = false;
    realmheart::services::LockSessionProvider provider;

    provider.set_unlock_callback([&callback_called]() {
        callback_called = true;
    });

    // unlock_session when not locked should NOT call the callback.
    provider.unlock_session();
    require(!callback_called, "unlock callback must NOT be called when not locked");
}

void test_unlock_callback_fires_when_locked() {
    bool callback_called = false;
    realmheart::services::LockSessionProvider provider;

    provider.set_unlock_callback([&callback_called]() {
        callback_called = true;
    });

    provider.mark_locked();
    provider.unlock_session();
    require(callback_called, "unlock callback must fire when locked");
    require(!provider.is_locked(), "must be unlocked after unlock");
}

void test_double_unlock_is_safe() {
    realmheart::services::LockSessionProvider provider;
    provider.mark_locked();
    provider.unlock_session();
    provider.unlock_session();  // Should not crash.
    require(!provider.is_locked(), "must still be unlocked after double unlock");
}

} // namespace

int main() {
    test_initial_state_unlocked();
    test_mark_locked_and_unlock();
    test_unlock_callback_only_when_locked();
    test_unlock_callback_fires_when_locked();
    test_double_unlock_is_safe();

    std::cout << "All LockSessionProvider tests PASSED\n";
    return 0;
}
