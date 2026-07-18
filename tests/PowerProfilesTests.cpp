#include "services/PowerProfiles.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class TemporaryFakePowerProfilesctl {
public:
    TemporaryFakePowerProfilesctl() {
        char pattern[] = "/tmp/realmheart-power-tests-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
        state_file_ = directory_ / "profile.state";
        std::ofstream(state_file_) << "balanced";

        const auto executable = directory_ / "powerprofilesctl";
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "if [ \"$REALMHEART_POWER_CLI_NOOP\" = 1 ]; then exit 0; fi\n"
               << "printf 'simulated broken Python wrapper\\n'\n"
               << "exit 1\n";
        script.close();
        ::chmod(executable.c_str(), 0700);

        const auto busctl = directory_ / "busctl";
        std::ofstream bus_script(busctl);
        bus_script << "#!/bin/sh\n"
                   << "case \"$1\" in\n"
                   << "  get-property) IFS= read -r value < \"$REALMHEART_POWER_TEST_STATE\"; printf 's \"%s\"\\n' \"$value\" ;;\n"
                   << "  set-property) if [ \"$REALMHEART_POWER_IGNORE_WRITES\" != 1 ]; then printf '%s' \"$7\" > \"$REALMHEART_POWER_TEST_STATE\"; fi ;;\n"
                   << "  *) exit 64 ;;\n"
                   << "esac\n";
        bus_script.close();
        ::chmod(busctl.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("REALMHEART_POWER_TEST_STATE", state_file_.c_str(), 1);
        ::unsetenv("REALMHEART_POWER_IGNORE_WRITES");
        ::unsetenv("REALMHEART_POWER_CLI_NOOP");
    }

    ~TemporaryFakePowerProfilesctl() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_POWER_TEST_STATE");
        ::unsetenv("REALMHEART_POWER_IGNORE_WRITES");
        ::unsetenv("REALMHEART_POWER_CLI_NOOP");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void use_successful_noop_cli() {
        ::setenv("REALMHEART_POWER_CLI_NOOP", "1", 1);
    }

    void ignore_writes() {
        ::setenv("REALMHEART_POWER_IGNORE_WRITES", "1", 1);
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
        TemporaryFakePowerProfilesctl fake;

        require(realmheart::services::PowerProfiles::current() == "balanced", "profile should be readable");
        fake.use_successful_noop_cli();
        require(realmheart::services::PowerProfiles::cycle() == "performance", "cycle should fall back when the CLI reports success without changing the profile");
        require(realmheart::services::PowerProfiles::current() == "performance", "profile should actually change");
        require(realmheart::services::PowerProfiles::cycle() == "power-saver", "cycle should use PPD's power-saver name");

        fake.ignore_writes();
        require(!realmheart::services::PowerProfiles::cycle(), "cycle must fail when readback does not change");
    } catch (const std::exception& error) {
        std::cerr << "PowerProfilesTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "PowerProfilesTests passed\n";
    return 0;
}
