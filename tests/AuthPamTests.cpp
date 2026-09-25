#include "ui/lockscreen/AuthPam.hpp"
#include "ui/lockscreen/AuthHelperPath.hpp"

#include <glib.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

using realmheart::ui::lockscreen::AuthPam;
using realmheart::ui::lockscreen::SecretBuffer;
using realmheart::ui::lockscreen::auth_helper_is_secure;
using realmheart::ui::lockscreen::auth_helper_path_candidates;
using realmheart::ui::lockscreen::first_secure_auth_helper_path;

void test_build_tree_finds_secure_system_helper_instead_of_sibling_binary() {
    const auto candidates = auth_helper_path_candidates(
        "/home/example/Realmheart/build-hybrid/realmheart",
        "libexec/realmheart/realmheart-auth-helper",
        "/home/example/.local/libexec/realmheart/realmheart-auth-helper"
    );
    const std::string system_helper =
        "/usr/local/libexec/realmheart/realmheart-auth-helper";

    assert(candidates.size() == 5);
    assert(candidates[2] == system_helper);
    assert(candidates[4] ==
        "/home/example/Realmheart/build-hybrid/realmheart-auth-helper");
    assert(first_secure_auth_helper_path(
        candidates,
        [&system_helper](const std::string& candidate) {
            return candidate == system_helper;
        }
    ) == system_helper);
    assert(first_secure_auth_helper_path(
        candidates,
        [](const std::string&) { return false; }
    ).empty());
    if (auth_helper_is_secure(system_helper)) {
        assert(first_secure_auth_helper_path(
            candidates,
            auth_helper_is_secure
        ) == system_helper);
    }
}

void test_recommended_installer_usr_local_layout_finds_auth_helper() {
    const std::string installed_helper =
        "/usr/local/libexec/realmheart/realmheart-auth-helper";
    const auto candidates = auth_helper_path_candidates(
        "/usr/local/bin/realmheart",
        "libexec/realmheart/realmheart-auth-helper",
        installed_helper
    );

    assert(!candidates.empty());
    assert(candidates.front() == installed_helper);
    assert(first_secure_auth_helper_path(
        candidates,
        [&installed_helper](const std::string& candidate) {
            return candidate == installed_helper;
        }
    ) == installed_helper);
}

void test_helper_security_metadata_contract() {
    const auto root = std::filesystem::temp_directory_path() /
        ("realmheart-auth-security-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto insecure = root / "user-owned-helper";
    {
        std::ofstream fixture(insecure);
        fixture << "#!/bin/sh\nexit 0\n";
    }
    std::filesystem::permissions(
        insecure,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace
    );
    assert(!auth_helper_is_secure(insecure.string()));

    const auto setuid_but_user_owned = root / "setuid-user-owned-helper";
    std::filesystem::copy_file(insecure, setuid_but_user_owned);
    std::filesystem::permissions(
        setuid_but_user_owned,
        std::filesystem::perms::set_uid,
        std::filesystem::perm_options::add
    );
    assert(!auth_helper_is_secure(setuid_but_user_owned.string()));

    const auto symlink = root / "helper-symlink";
    std::filesystem::create_symlink("/usr/bin/passwd", symlink);
    assert(!auth_helper_is_secure(symlink.string()));

    struct stat passwd_metadata{};
    assert(::stat("/usr/bin/passwd", &passwd_metadata) == 0);
    assert(auth_helper_is_secure("/usr/bin/passwd"));
    std::filesystem::remove_all(root);
}

int main() {
    test_build_tree_finds_secure_system_helper_instead_of_sibling_binary();
    test_recommended_installer_usr_local_layout_finds_auth_helper();
    test_helper_security_metadata_contract();
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
