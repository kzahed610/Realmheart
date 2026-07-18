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

class TemporaryFakeHyprsunset {
public:
    TemporaryFakeHyprsunset() {
        char pattern[] = "/tmp/realmheart-night-light-tests-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
        running_file_ = directory_ / "running.state";
        applied_file_ = directory_ / "applied.state";
        std::ofstream(running_file_) << "no";

        const auto hyprctl = directory_ / "hyprctl";
        std::ofstream script(hyprctl);
        script << "#!/bin/sh\n"
               << "IFS= read -r running < \"$REALMHEART_NIGHT_TEST_RUNNING\"\n"
               << "if [ \"$running\" != yes ]; then printf 'not running\\n'; exit 1; fi\n"
               << "if [ \"$REALMHEART_NIGHT_FAIL_WRITES\" = 1 ]; then printf 'forced failure\\n'; exit 1; fi\n"
               << "case \"$*\" in\n"
               << "  'hyprsunset identity') printf 'off\\n' > \"$REALMHEART_NIGHT_TEST_APPLIED\"; printf 'ok\\n' ;;\n"
               << "  'hyprsunset temperature '*) printf '%s\\n' \"$3\" > \"$REALMHEART_NIGHT_TEST_APPLIED\"; printf 'ok\\n' ;;\n"
               << "  *) exit 64 ;;\n"
               << "esac\n";
        script.close();
        ::chmod(hyprctl.c_str(), 0700);

        const auto hyprsunset = directory_ / "hyprsunset";
        std::ofstream(hyprsunset) << "#!/bin/sh\nexit 0\n";
        ::chmod(hyprsunset.c_str(), 0700);

        const auto systemctl = directory_ / "systemctl";
        std::ofstream start_script(systemctl);
        start_script << "#!/bin/sh\n"
                     << "if [ \"$*\" != '--user start hyprsunset.service' ]; then exit 64; fi\n"
                     << "printf yes > \"$REALMHEART_NIGHT_TEST_RUNNING\"\n";
        start_script.close();
        ::chmod(systemctl.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        const char* old_runtime = std::getenv("XDG_RUNTIME_DIR");
        old_runtime_ = old_runtime != nullptr ? old_runtime : "";
        had_runtime_ = old_runtime != nullptr;

        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("XDG_RUNTIME_DIR", directory_.c_str(), 1);
        ::setenv("REALMHEART_NIGHT_TEST_RUNNING", running_file_.c_str(), 1);
        ::setenv("REALMHEART_NIGHT_TEST_APPLIED", applied_file_.c_str(), 1);
        ::unsetenv("REALMHEART_NIGHT_FAIL_WRITES");
    }

    ~TemporaryFakeHyprsunset() {
        ::setenv("PATH", old_path_.c_str(), 1);
        if (had_runtime_) ::setenv("XDG_RUNTIME_DIR", old_runtime_.c_str(), 1);
        else ::unsetenv("XDG_RUNTIME_DIR");
        ::unsetenv("REALMHEART_NIGHT_TEST_RUNNING");
        ::unsetenv("REALMHEART_NIGHT_TEST_APPLIED");
        ::unsetenv("REALMHEART_NIGHT_FAIL_WRITES");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void fail_writes() {
        ::setenv("REALMHEART_NIGHT_FAIL_WRITES", "1", 1);
    }

    [[nodiscard]] std::string applied() const {
        std::ifstream input(applied_file_);
        std::string value;
        input >> value;
        return value;
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path running_file_;
    std::filesystem::path applied_file_;
    std::string old_path_;
    std::string old_runtime_;
    bool had_runtime_ = false;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        TemporaryFakeHyprsunset fake;

        const auto initial = realmheart::services::NightLight::read();
        require(
            initial && !initial->enabled &&
                initial->temperature == realmheart::services::NightLight::kDefaultTemperature,
            "Night Light should begin available and off"
        );

        const auto enabled = realmheart::services::NightLight::set_enabled(true);
        require(enabled.success && enabled.state.enabled, "Night Light enable should succeed");
        require(fake.applied() == "4000", "enable should apply the remembered temperature");

        const auto warmer = realmheart::services::NightLight::set_temperature(2750);
        require(
            warmer.success && warmer.state.enabled && warmer.state.temperature == 2750,
            "temperature mutation should succeed"
        );
        require(fake.applied() == "2750", "temperature should be sent through official IPC");

        const auto remembered = realmheart::services::NightLight::read();
        require(
            remembered && remembered->enabled && remembered->temperature == 2750,
            "successful state should be remembered"
        );

        const auto disabled = realmheart::services::NightLight::set_enabled(false);
        require(disabled.success && !disabled.state.enabled, "Night Light disable should succeed");
        require(disabled.state.temperature == 2750, "disable should retain chosen strength");
        require(fake.applied() == "off", "disable should send identity IPC");

        require(
            realmheart::services::NightLight::strength_to_temperature(0) == 6000 &&
                realmheart::services::NightLight::strength_to_temperature(100) == 2500,
            "strength conversion should cover the complete range"
        );
        require(
            realmheart::services::NightLight::temperature_to_strength(6000) == 0 &&
                realmheart::services::NightLight::temperature_to_strength(2500) == 100,
            "temperature conversion should invert the strength range"
        );

        fake.fail_writes();
        const auto failed = realmheart::services::NightLight::set_enabled(true);
        require(!failed.success, "failed IPC writes must be reported");
    } catch (const std::exception& error) {
        std::cerr << "NightLightTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "NightLightTests passed\n";
    return 0;
}
