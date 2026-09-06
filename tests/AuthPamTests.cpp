#include "ui/lockscreen/AuthPam.hpp"

#include <glib.h>

#include <cassert>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

using realmheart::ui::lockscreen::AuthPam;
using realmheart::ui::lockscreen::SecretBuffer;

int main() {
    SecretBuffer secret("correct horse battery staple");
    assert(secret.valid());
    assert(secret.size() == 28);
    assert(std::string(secret.data(), secret.size()) ==
           "correct horse battery staple");

    SecretBuffer moved(std::move(secret));
    assert(moved.valid());
    assert(moved.size() == 28);
    assert(secret.empty());

    SecretBuffer reassigned("temporary");
    reassigned = std::move(moved);
    assert(reassigned.valid());
    assert(reassigned.size() == 28);
    assert(moved.empty());

    const std::string overlong(AuthPam::kMaxPasswordBytes + 1, 'x');
    SecretBuffer rejected(overlong);
    assert(!rejected.valid());
    assert(rejected.empty());

    AuthPam auth;
    int callback_count = 0;
    bool callback_success = true;
    auth.verify_async(
        "__realmheart_protocol_test_invalid__",
        SecretBuffer("not-used-by-missing-helper"),
        [&](bool success) {
            ++callback_count;
            callback_success = success;
        }
    );
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(1);
    while (callback_count == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_iteration(nullptr, FALSE)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(callback_count == 1);
    assert(!callback_success);
    auth.cancel();
    return 0;
}
