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
        daemon_file_ = directory_ / "daemon.state";
        temperature_file_ = directory_ / "temperature.state";
        applied_file_ = directory_ / "applied.state";
        std::ofstream(running_file_) << "no";
        std::ofstream(daemon_file_) << "no";
        std::ofstream(temperature_file_) << "4000";

        const auto hyprctl = directory_ / "hyprctl";
        std::ofstream script(hyprctl);
        script << "#!/bin/sh\n"
               << "IFS= read -r running < \"$REALMHEART_NIGHT_TEST_RUNNING\"\n"
               << "if [ \"$running\" != yes ]; then printf 'not running\\n'; exit 1; fi\n"
               << "if [ \"$1\" = hyprsunset ]; then IFS= read -r daemon < \"$REALMHEART_NIGHT_TEST_DAEMON\"; if [ \"$daemon\" != yes ]; then printf 'daemon unavailable\\n'; exit 1; fi; fi\n"
               << "if [ \"$REALMHEART_NIGHT_FAIL_WRITES\" = 1 ]; then printf 'forced failure\\n'; exit 1; fi\n"
               << "case \"$*\" in\n"
               << "  'monitors -j') printf '[]' ;;\n"
               << "  'hyprsunset temperature') IFS= read -r temperature < \"$REALMHEART_NIGHT_TEST_TEMPERATURE\"; printf '%s\\n' \"$temperature\" ;;\n"
               << "  'hyprsunset identity') printf 'off\\n' > \"$REALMHEART_NIGHT_TEST_APPLIED\"; printf 'ok\\n' ;;\n"
               << "  'hyprsunset temperature '*) printf '%s\\n' \"$3\" > \"$REALMHEART_NIGHT_TEST_APPLIED\"; printf '%s\\n' \"$3\" > \"$REALMHEART_NIGHT_TEST_TEMPERATURE\"; printf 'ok\\n' ;;\n"
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
                     << "if [ \"$REALMHEART_NIGHT_TEST_FAIL_START\" = 1 ]; then exit 1; fi\n"
                     << "printf yes > \"$REALMHEART_NIGHT_TEST_RUNNING\"\n"
                     << "printf yes > \"$REALMHEART_NIGHT_TEST_DAEMON\"\n";
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
        ::setenv("REALMHEART_NIGHT_TEST_DAEMON", daemon_file_.c_str(), 1);
        ::setenv("REALMHEART_NIGHT_TEST_TEMPERATURE", temperature_file_.c_str(), 1);
        ::setenv("REALMHEART_NIGHT_TEST_APPLIED", applied_file_.c_str(), 1);
        ::unsetenv("REALMHEART_NIGHT_FAIL_WRITES");
        ::unsetenv("REALMHEART_NIGHT_TEST_FAIL_START");
    }

    ~TemporaryFakeHyprsunset() {
        ::setenv("PATH", old_path_.c_str(), 1);
        if (had_runtime_) ::setenv("XDG_RUNTIME_DIR", old_runtime_.c_str(), 1);
        else ::unsetenv("XDG_RUNTIME_DIR");
        ::unsetenv("REALMHEART_NIGHT_TEST_RUNNING");
        ::unsetenv("REALMHEART_NIGHT_TEST_DAEMON");
        ::unsetenv("REALMHEART_NIGHT_TEST_TEMPERATURE");
        ::unsetenv("REALMHEART_NIGHT_TEST_APPLIED");
        ::unsetenv("REALMHEART_NIGHT_FAIL_WRITES");
        ::unsetenv("REALMHEART_NIGHT_TEST_FAIL_START");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void fail_writes() {
        ::setenv("REALMHEART_NIGHT_FAIL_WRITES", "1", 1);
    }

    void allow_writes() {
        ::unsetenv("REALMHEART_NIGHT_FAIL_WRITES");
    }

    void fail_start() {
        ::setenv("REALMHEART_NIGHT_TEST_FAIL_START", "1", 1);
    }

    void allow_start() {
        ::unsetenv("REALMHEART_NIGHT_TEST_FAIL_START");
    }

    void stop_daemon() const {
        std::ofstream(daemon_file_, std::ios::trunc) << "no\n";
    }

    void start_daemon() const {
        std::ofstream(daemon_file_, std::ios::trunc) << "yes\n";
    }

    void set_daemon_temperature(int temperature) const {
        std::ofstream(temperature_file_, std::ios::trunc) << temperature << '\n';
    }

    void block_persistence() {
        std::error_code error;
        std::filesystem::remove(directory_ / "realmheart-night-light.state", error);
        std::filesystem::create_directory(directory_ / "realmheart-night-light.state", error);
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
    std::filesystem::path daemon_file_;
    std::filesystem::path temperature_file_;
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
        require(!initial, "Night Light must report unavailable before a live daemon session exists");
        require(
            realmheart::services::NightLight::recovery_available(),
            "installed Night Light backend must expose a recovery action when its daemon is stopped"
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

        fake.set_daemon_temperature(3000);
        require(!realmheart::services::NightLight::read(),
                "externally changed daemon temperature must invalidate remembered state");
        fake.set_daemon_temperature(2750);

        fake.stop_daemon();
        require(!realmheart::services::NightLight::read(),
                "persisted Night Light state must be unavailable when the daemon is dead");

        fake.fail_start();
        const auto failed_recovery = realmheart::services::NightLight::set_enabled(true);
        require(!failed_recovery.success, "failed daemon recovery must be reported");
        require(!realmheart::services::NightLight::read(),
                "failed daemon recovery must not fabricate a live state");

        fake.allow_start();
        const auto recovered = realmheart::services::NightLight::set_enabled(true);
        require(recovered.success && recovered.state.enabled,
                "an installed but inactive daemon must be recoverable through the mutation path");
        const auto recovered_live = realmheart::services::NightLight::read();
        require(recovered_live && recovered_live->enabled,
                "successful recovery must be followed by verified live state");

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

        fake.allow_writes();
        fake.block_persistence();
        const auto persistence_failure = realmheart::services::NightLight::set_enabled(true);
        require(!persistence_failure.success && persistence_failure.partial,
                "successful IPC with failed persistence must be observable");
    } catch (const std::exception& error) {
        std::cerr << "NightLightTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "NightLightTests passed\n";
    return 0;
}
