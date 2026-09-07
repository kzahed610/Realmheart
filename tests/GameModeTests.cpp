#include "services/GameMode.hpp"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

class TemporaryFakeHyprctl {
public:
    TemporaryFakeHyprctl() {
        char pattern[] = "/tmp/realmheart-gamemode-tests-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
        compositor_state_ = directory_ / "hyprland.json";
        marker_state_ = directory_ / "realmheart-gamemode.json";
        write_compositor_state(true);

        const auto executable = directory_ / "hyprctl";
        std::ofstream script(executable);
        script << R"PY(#!/usr/bin/python3
import json
import os
import sys

path = os.environ["REALMHEART_GAME_TEST_STATE"]
with open(path, "r", encoding="utf-8") as handle:
    state = json.load(handle)

args = sys.argv[1:]
if len(args) >= 3 and args[0] == "getoption":
    name = args[1]
    value = state[name]
    if name in {
        "animations:enabled", "decoration:shadow:enabled",
        "decoration:blur:enabled", "general:allow_tearing"
    }:
        print(json.dumps({"option": name, "bool": bool(int(value))}))
    else:
        print(json.dumps({"option": name, "int": int(value)}))
    raise SystemExit(0)

if len(args) >= 2 and args[0] == "--batch":
    delay = os.environ.get("REALMHEART_GAME_BATCH_DELAY")
    if delay: import time; time.sleep(float(delay))
    if os.environ.get("REALMHEART_GAME_IGNORE_WRITES") != "1":
        for command in args[1].split(";"):
            pieces = command.strip().split(maxsplit=2)
            if len(pieces) == 3 and pieces[0] == "keyword":
                state[pieces[1]] = pieces[2]
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(state, handle)
    print("ok")
    raise SystemExit(0)

print("unexpected arguments", args, file=sys.stderr)
raise SystemExit(64)
)PY";
        script.close();
        ::chmod(executable.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("REALMHEART_GAME_TEST_STATE", compositor_state_.c_str(), 1);
        ::setenv("REALMHEART_GAMEMODE_STATE", marker_state_.c_str(), 1);
        ::unsetenv("REALMHEART_GAME_IGNORE_WRITES");
        ::unsetenv("REALMHEART_GAME_BATCH_DELAY");
    }

    ~TemporaryFakeHyprctl() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_GAME_TEST_STATE");
        ::unsetenv("REALMHEART_GAMEMODE_STATE");
        ::unsetenv("REALMHEART_GAME_IGNORE_WRITES");
        ::unsetenv("REALMHEART_GAME_BATCH_DELAY");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void ignore_writes(bool ignore = true) {
        if (ignore) ::setenv("REALMHEART_GAME_IGNORE_WRITES", "1", 1);
        else ::unsetenv("REALMHEART_GAME_IGNORE_WRITES");
    }

    void allow_writes() { ::unsetenv("REALMHEART_GAME_IGNORE_WRITES"); }

    void delay_batches(double seconds) {
        const auto value = std::to_string(seconds);
        ::setenv("REALMHEART_GAME_BATCH_DELAY", value.c_str(), 1);
    }

    void write_compositor_state(bool animations_enabled) {
        std::ofstream state(compositor_state_, std::ios::trunc);
        state << "{\n"
              << "  \"animations:enabled\": \"" << (animations_enabled ? "1" : "0") << "\",\n"
              << "  \"decoration:shadow:enabled\": \"1\",\n"
              << "  \"decoration:blur:enabled\": \"1\",\n"
              << "  \"general:gaps_in\": \"5\",\n"
              << "  \"general:gaps_out\": \"10\",\n"
              << "  \"general:border_size\": \"2\",\n"
              << "  \"decoration:rounding\": \"8\",\n"
              << "  \"general:allow_tearing\": \"0\"\n"
              << "}\n";
    }

    std::string compositor_contents() const {
        std::ifstream state(compositor_state_);
        return {std::istreambuf_iterator<char>(state), std::istreambuf_iterator<char>()};
    }

    bool marker_exists() const { return std::filesystem::exists(marker_state_); }

    void write_marker(const std::string& contents) const {
        std::ofstream marker(marker_state_, std::ios::trunc);
        marker << contents;
        ::chmod(marker_state_.c_str(), 0600);
    }

    void clear_marker() const {
        std::error_code error;
        std::filesystem::remove(marker_state_, error);
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path compositor_state_;
    std::filesystem::path marker_state_;
    std::string old_path_;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main(int argc, char** argv) {
    static_cast<void>(argc);
    if (std::getenv("REALMHEART_GAME_CHILD") != nullptr) {
        const auto result = realmheart::services::GameMode::set_enabled(true);
        return result.success ? 0 : 1;
    }

    try {
        TemporaryFakeHyprctl fake;

        const auto initial = realmheart::services::GameMode::read();
        require(initial && !initial->enabled, "Gamemode should start disabled without Realmheart ownership");

        const auto enabled = realmheart::services::GameMode::set_enabled(true);
        require(enabled.success && enabled.state.enabled, "Gamemode enable should pass readback");
        require(fake.marker_exists(), "Gamemode enable should persist a restoration snapshot");
        const auto active = fake.compositor_contents();
        require(active.find("\"general:gaps_in\": \"0\"") != std::string::npos,
                "Gamemode should apply every configured override");

        const auto disabled = realmheart::services::GameMode::set_enabled(false);
        require(disabled.success && !disabled.state.enabled, "Gamemode disable should restore its snapshot");
        require(!fake.marker_exists(), "Successful restore should remove the ownership marker");
        const auto restored = fake.compositor_contents();
        require(restored.find("\"general:gaps_in\": \"5\"") != std::string::npos,
                "Gamemode should restore the original gaps value");
        require(restored.find("\"decoration:rounding\": \"8\"") != std::string::npos,
                "Gamemode should restore the original rounding value");

        // A user who normally disables animations must not be misidentified as
        // having enabled Realmheart's Gamemode.
        fake.write_compositor_state(false);
        const auto user_disabled_animations = realmheart::services::GameMode::read();
        require(user_disabled_animations && !user_disabled_animations->enabled,
                "Animations disabled by the user must not imply Realmheart Gamemode ownership");
        fake.write_compositor_state(true);

        fake.write_marker(R"({"version":1,"token":7,"options":{"animations:enabled":"0; keyword general:gaps_in 99"}})" );
        require(!realmheart::services::GameMode::set_enabled(false).success,
                "malformed restoration values must be rejected before batch construction");
        fake.clear_marker();
        fake.write_marker(std::string(17000, 'x'));
        const auto oversized = realmheart::services::GameMode::read();
        require(oversized && !oversized->enabled, "oversized restoration files must be ignored");
        fake.clear_marker();

        fake.ignore_writes();
        const auto mismatch = realmheart::services::GameMode::set_enabled(true);
        require(!mismatch.success, "Gamemode readback mismatch must fail");
        require(!fake.marker_exists(), "Failed enable must clean up its restoration marker");

        fake.allow_writes();
        fake.write_compositor_state(true);
        fake.write_marker(R"json({"version":2,"token":7,"owner":"foreign-process:1","options":{"animations:enabled":"1","decoration:shadow:enabled":"1","decoration:blur:enabled":"1","general:gaps_in":"5","general:gaps_out":"10","general:border_size":"2","decoration:rounding":"8","general:allow_tearing":"0"}})json");
        const auto foreign_disable = realmheart::services::GameMode::set_enabled(false);
        require(!foreign_disable.success,
                "a process must not restore a foreign Gamemode snapshot");
        require(fake.marker_exists(), "foreign ownership marker must remain durable");
        fake.clear_marker();

        fake.delay_batches(0.25);
        const pid_t child = ::fork();
        require(child >= 0, "cross-process Gamemode child must fork");
        if (child == 0) {
            ::setenv("REALMHEART_GAME_CHILD", "enable", 1);
            ::execl(argv[0], argv[0], static_cast<char*>(nullptr));
            _exit(127);
        }
        bool child_started = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (fake.marker_exists()) {
                child_started = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(child_started, "cross-process child must publish its snapshot before contention");
        const auto contention_started = std::chrono::steady_clock::now();
        const auto concurrent_disable = realmheart::services::GameMode::set_enabled(false);
        const auto contention_elapsed = std::chrono::steady_clock::now() - contention_started;
        int child_status = 0;
        require(::waitpid(child, &child_status, 0) == child,
                "cross-process Gamemode child must be reaped");
        require(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
                "cross-process Gamemode enable must complete successfully");
        require(contention_elapsed >= std::chrono::milliseconds(150),
                "cross-process Gamemode operations must serialize on the durable lock");
        require(!concurrent_disable.success,
                "a separate Realmheart instance must not restore another instance's snapshot");
        require(fake.marker_exists(),
                "cross-process ownership marker must survive a foreign disable attempt");
        fake.clear_marker();
    } catch (const std::exception& error) {
        std::cerr << "GameModeTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "GameModeTests passed\n";
    return 0;
}
