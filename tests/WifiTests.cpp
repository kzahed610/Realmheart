#include "services/Wifi.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class TemporaryFakeNmcli {
public:
    TemporaryFakeNmcli() {
        char pattern[] = "/tmp/realmheart-wifi-tests-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
        state_file_ = directory_ / "wifi.state";
        write_state("enabled");

        const auto executable = directory_ / "nmcli";
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "case \"$*\" in\n"
               << "  'radio wifi') IFS= read -r state < \"$REALMHEART_WIFI_TEST_STATE\"; printf '%s\\n' \"$state\" ;;\n"
               << "  'radio wifi on') if [ \"$REALMHEART_WIFI_IGNORE_WRITES\" != 1 ]; then printf enabled > \"$REALMHEART_WIFI_TEST_STATE\"; fi ;;\n"
               << "  'radio wifi off') if [ \"$REALMHEART_WIFI_IGNORE_WRITES\" != 1 ]; then printf disabled > \"$REALMHEART_WIFI_TEST_STATE\"; fi ;;\n"
               << "  '-t -f ACTIVE,SSID dev wifi') printf 'yes:RealmNet\\n' ;;\n"
               << "  *) printf 'unexpected arguments: %s\\n' \"$*\"; exit 64 ;;\n"
               << "esac\n";
        script.close();
        ::chmod(executable.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("REALMHEART_WIFI_TEST_STATE", state_file_.c_str(), 1);
        ::unsetenv("REALMHEART_WIFI_IGNORE_WRITES");
    }

    ~TemporaryFakeNmcli() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_WIFI_TEST_STATE");
        ::unsetenv("REALMHEART_WIFI_IGNORE_WRITES");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void ignore_writes(bool enabled) {
        if (enabled) ::setenv("REALMHEART_WIFI_IGNORE_WRITES", "1", 1);
        else ::unsetenv("REALMHEART_WIFI_IGNORE_WRITES");
    }

private:
    void write_state(const std::string& state) {
        std::ofstream output(state_file_);
        output << state;
    }

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
        TemporaryFakeNmcli fake;

        const auto initial = realmheart::services::Wifi::read();
        require(initial.has_value(), "WiFi state should be readable");
        require(initial->enabled, "WiFi should start enabled");
        require(initial->ssid == "RealmNet", "active SSID should be captured");

        const auto disabled = realmheart::services::Wifi::set_enabled(false);
        require(disabled.success, "disabling WiFi should succeed after matching readback");
        require(!disabled.state.enabled, "readback should report WiFi disabled");

        const auto enabled = realmheart::services::Wifi::set_enabled(true);
        require(enabled.success, "enabling WiFi should succeed after matching readback");
        require(enabled.state.enabled, "readback should report WiFi enabled");

        fake.ignore_writes(true);
        const auto mismatch = realmheart::services::Wifi::set_enabled(false);
        require(!mismatch.success, "readback mismatch must fail the mutation");
        require(mismatch.state.enabled, "failed mutation should expose actual readback state");
    } catch (const std::exception& error) {
        std::cerr << "WifiTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "WifiTests passed\n";
    return 0;
}
