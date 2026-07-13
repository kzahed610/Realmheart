#include "services/GameMode.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
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
    }

    ~TemporaryFakeHyprctl() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_GAME_TEST_STATE");
        ::unsetenv("REALMHEART_GAMEMODE_STATE");
        ::unsetenv("REALMHEART_GAME_IGNORE_WRITES");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void ignore_writes(bool ignore = true) {
        if (ignore) ::setenv("REALMHEART_GAME_IGNORE_WRITES", "1", 1);
        else ::unsetenv("REALMHEART_GAME_IGNORE_WRITES");
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

int main() {
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

        fake.ignore_writes();
        const auto mismatch = realmheart::services::GameMode::set_enabled(true);
        require(!mismatch.success, "Gamemode readback mismatch must fail");
        require(!fake.marker_exists(), "Failed enable must clean up its restoration marker");
    } catch (const std::exception& error) {
        std::cerr << "GameModeTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "GameModeTests passed\n";
    return 0;
}
