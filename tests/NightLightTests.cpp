#include "services/NightLight.hpp"

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
        char pattern[] = "/tmp/realmheart-night-light-tests-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
        state_file_ = directory_ / "temperature.state";
        std::ofstream(state_file_) << "6500";

        const auto executable = directory_ / "hyprctl";
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "if [ \"$1 $2\" != 'hyprsunset temperature' ]; then exit 64; fi\n"
               << "if [ -z \"$3\" ]; then IFS= read -r value < \"$REALMHEART_NIGHT_TEST_STATE\"; printf '%s\\n' \"$value\"; exit 0; fi\n"
               << "if [ \"$REALMHEART_NIGHT_IGNORE_WRITES\" != 1 ]; then printf '%s' \"$3\" > \"$REALMHEART_NIGHT_TEST_STATE\"; fi\n"
               << "printf 'ok\\n'\n";
        script.close();
        ::chmod(executable.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("REALMHEART_NIGHT_TEST_STATE", state_file_.c_str(), 1);
        ::unsetenv("REALMHEART_NIGHT_IGNORE_WRITES");
    }

    ~TemporaryFakeHyprctl() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_NIGHT_TEST_STATE");
        ::unsetenv("REALMHEART_NIGHT_IGNORE_WRITES");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void ignore_writes() {
        ::setenv("REALMHEART_NIGHT_IGNORE_WRITES", "1", 1);
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

        const auto initial = realmheart::services::NightLight::read();
        require(initial && !initial->enabled && initial->temperature == 6500, "Night Light should start off");

        const auto enabled = realmheart::services::NightLight::set_enabled(true);
        require(enabled.success && enabled.state.enabled, "Night Light enable should pass readback");
        require(enabled.state.temperature == 4000, "Night Light should use 4000K");

        const auto disabled = realmheart::services::NightLight::set_enabled(false);
        require(disabled.success && !disabled.state.enabled, "Night Light disable should pass readback");
        require(disabled.state.temperature == 6500, "Night Light off should use 6500K");

        fake.ignore_writes();
        const auto mismatch = realmheart::services::NightLight::set_enabled(true);
        require(!mismatch.success, "Night Light readback mismatch must fail");
    } catch (const std::exception& error) {
        std::cerr << "NightLightTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "NightLightTests passed\n";
    return 0;
}
