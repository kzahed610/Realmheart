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
        state_file_ = directory_ / "animations.state";
        std::ofstream(state_file_) << "true";

        const auto executable = directory_ / "hyprctl";
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "if [ \"$1\" = getoption ]; then IFS= read -r value < \"$REALMHEART_GAME_TEST_STATE\"; printf '{\"option\":\"animations:enabled\",\"bool\":%s}\\n' \"$value\"; exit 0; fi\n"
               << "if [ \"$REALMHEART_GAME_IGNORE_WRITES\" != 1 ]; then\n"
               << "  [ \"$1\" = --batch ] && printf false > \"$REALMHEART_GAME_TEST_STATE\"\n"
               << "  [ \"$1\" = reload ] && printf true > \"$REALMHEART_GAME_TEST_STATE\"\n"
               << "fi\n"
               << "printf 'ok\\n'\n";
        script.close();
        ::chmod(executable.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("REALMHEART_GAME_TEST_STATE", state_file_.c_str(), 1);
        ::unsetenv("REALMHEART_GAME_IGNORE_WRITES");
    }

    ~TemporaryFakeHyprctl() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_GAME_TEST_STATE");
        ::unsetenv("REALMHEART_GAME_IGNORE_WRITES");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void ignore_writes() {
        ::setenv("REALMHEART_GAME_IGNORE_WRITES", "1", 1);
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path state_file_;
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
        require(initial && !initial->enabled, "Gamemode should start disabled when animations are on");

        const auto enabled = realmheart::services::GameMode::set_enabled(true);
        require(enabled.success && enabled.state.enabled, "Gamemode enable should pass readback");

        const auto disabled = realmheart::services::GameMode::set_enabled(false);
        require(disabled.success && !disabled.state.enabled, "Gamemode disable should reload and pass readback");

        fake.ignore_writes();
        const auto mismatch = realmheart::services::GameMode::set_enabled(true);
        require(!mismatch.success, "Gamemode readback mismatch must fail");
    } catch (const std::exception& error) {
        std::cerr << "GameModeTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "GameModeTests passed\n";
    return 0;
}
