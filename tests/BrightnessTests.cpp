#include "services/Brightness.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class TemporaryFakeBrightnessctl {
public:
    TemporaryFakeBrightnessctl() {
        char pattern[] = "/tmp/realmheart-brightness-tests-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
        state_file_ = directory_ / "brightness.state";
        std::ofstream(state_file_) << "400";

        const auto executable = directory_ / "brightnessctl";
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "case \"$1\" in\n"
               << "  g) IFS= read -r value < \"$REALMHEART_BRIGHTNESS_TEST_STATE\"; printf '%s\\n' \"$value\" ;;\n"
               << "  m) printf '1000\\n' ;;\n"
               << "  set) if [ \"$REALMHEART_BRIGHTNESS_IGNORE_WRITES\" != 1 ]; then percent=${2%\\%}; printf '%s' \"$((percent * 10))\" > \"$REALMHEART_BRIGHTNESS_TEST_STATE\"; fi ;;\n"
               << "  *) exit 64 ;;\n"
               << "esac\n";
        script.close();
        ::chmod(executable.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("REALMHEART_BRIGHTNESS_TEST_STATE", state_file_.c_str(), 1);
        ::unsetenv("REALMHEART_BRIGHTNESS_IGNORE_WRITES");
    }

    ~TemporaryFakeBrightnessctl() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_BRIGHTNESS_TEST_STATE");
        ::unsetenv("REALMHEART_BRIGHTNESS_IGNORE_WRITES");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void ignore_writes() {
        ::setenv("REALMHEART_BRIGHTNESS_IGNORE_WRITES", "1", 1);
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
        TemporaryFakeBrightnessctl fake;

        const auto initial = realmheart::services::Brightness::read();
        require(initial.has_value(), "brightness should be readable");
        require(initial->percent == 40.0, "brightness percentage should be parsed");

        require(realmheart::services::Brightness::set(65), "matching brightness readback should succeed");
        const auto changed = realmheart::services::Brightness::read();
        require(changed && changed->percent == 65.0, "brightness should change to 65%");

        require(realmheart::services::Brightness::set(200), "clamped brightness write should succeed");
        const auto clamped = realmheart::services::Brightness::read();
        require(clamped && clamped->percent == 100.0, "brightness should clamp to 100%");

        fake.ignore_writes();
        require(!realmheart::services::Brightness::set(20), "readback mismatch must fail brightness mutation");
    } catch (const std::exception& error) {
        std::cerr << "BrightnessTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "BrightnessTests passed\n";
    return 0;
}
