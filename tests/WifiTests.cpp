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
        active_file_ = directory_ / "wifi.active";
        write_state("enabled");
        std::ofstream(active_file_) << "Realm:Net";

        const auto executable = directory_ / "nmcli";
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "case \"$*\" in\n"
               << "  'radio wifi') IFS= read -r state < \"$REALMHEART_WIFI_TEST_STATE\"; printf '%s\\n' \"$state\" ;;\n"
               << "  'radio wifi on') if [ \"$REALMHEART_WIFI_IGNORE_WRITES\" != 1 ]; then printf enabled > \"$REALMHEART_WIFI_TEST_STATE\"; fi ;;\n"
               << "  'radio wifi off') if [ \"$REALMHEART_WIFI_IGNORE_WRITES\" != 1 ]; then printf disabled > \"$REALMHEART_WIFI_TEST_STATE\"; fi ;;\n"
               << "  '-t -f IN-USE,SSID,SIGNAL device wifi list --rescan no') IFS= read -r active < \"$REALMHEART_WIFI_TEST_ACTIVE\"; if [ \"$REALMHEART_WIFI_HIDE_IN_USE\" = 1 ]; then exit 0; fi; case \"$active\" in 'Realm:Net') printf '*:Realm\\:Net:67\\n' ;; OpenNet) printf '*:OpenNet:81\\n' ;; SecretNet) printf '*:SecretNet:74\\n' ;; esac ;;\n"
               << "  '-t -f IN-USE,SSID,BSSID,SIGNAL,SECURITY device wifi list --rescan yes'|'-t -f IN-USE,SSID,BSSID,SIGNAL,SECURITY device wifi list --rescan no') IFS= read -r active < \"$REALMHEART_WIFI_TEST_ACTIVE\"; if [ -n \"$active\" ]; then printf '*:Realm\\:Net:AA\\:BB\\:CC\\:DD\\:EE\\:01:67:WPA2\\n'; else printf ':Realm\\:Net:AA\\:BB\\:CC\\:DD\\:EE\\:01:67:WPA2\\n'; fi; printf ':Realm\\:Net:AA\\:BB\\:CC\\:DD\\:EE\\:02:41:WPA2\\n:OpenNet:AA\\:BB\\:CC\\:DD\\:EE\\:03:81:--\\n:SecretNet:AA\\:BB\\:CC\\:DD\\:EE\\:04:74:WPA2\\n' ;;\n"
               << "  '-t -f NAME,UUID,TYPE connection show') printf 'Realm\\:Net:uuid-realm:802-11-wireless\\n' ;;\n"
               << "  '-t -f DEVICE,TYPE,STATE device') IFS= read -r active < \"$REALMHEART_WIFI_TEST_ACTIVE\"; if [ -n \"$active\" ]; then printf 'wlan0:wifi:connected\\n'; else printf 'wlan0:wifi:disconnected\\n'; fi ;;\n"
               << "  '-t -f DEVICE,TYPE,STATE,CONNECTION device status') IFS= read -r active < \"$REALMHEART_WIFI_TEST_ACTIVE\"; case \"$active\" in 'Realm:Net') printf 'wlan0:wifi:connected:Realm\\:Net\\n' ;; OpenNet) printf 'wlan0:wifi:connected:OpenNet\\n' ;; SecretNet) printf 'wlan0:wifi:connected:SecretNet\\n' ;; *) printf 'wlan0:wifi:disconnected:--\\n' ;; esac ;;\n"
               << "  'connection up uuid uuid-realm') printf 'Realm:Net' > \"$REALMHEART_WIFI_TEST_ACTIVE\" ;;\n"
               << "  'device wifi connect OpenNet') printf 'OpenNet' > \"$REALMHEART_WIFI_TEST_ACTIVE\" ;;\n"
               << "  'device wifi connect SecretNet password aether-key') printf 'SecretNet' > \"$REALMHEART_WIFI_TEST_ACTIVE\" ;;\n"
               << "  'device disconnect wlan0') printf '' > \"$REALMHEART_WIFI_TEST_ACTIVE\" ;;\n"
               << "  'connection delete uuid uuid-realm') : ;;\n"
               << "  *) printf 'unexpected arguments: %s\\n' \"$*\"; exit 64 ;;\n"
               << "esac\n";
        script.close();
        ::chmod(executable.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("REALMHEART_WIFI_TEST_STATE", state_file_.c_str(), 1);
        ::setenv("REALMHEART_WIFI_TEST_ACTIVE", active_file_.c_str(), 1);
        ::unsetenv("REALMHEART_WIFI_IGNORE_WRITES");
        ::unsetenv("REALMHEART_WIFI_HIDE_IN_USE");
    }

    ~TemporaryFakeNmcli() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_WIFI_TEST_STATE");
        ::unsetenv("REALMHEART_WIFI_TEST_ACTIVE");
        ::unsetenv("REALMHEART_WIFI_IGNORE_WRITES");
        ::unsetenv("REALMHEART_WIFI_HIDE_IN_USE");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void ignore_writes(bool enabled) {
        if (enabled) ::setenv("REALMHEART_WIFI_IGNORE_WRITES", "1", 1);
        else ::unsetenv("REALMHEART_WIFI_IGNORE_WRITES");
    }

    void hide_in_use_marker(bool enabled) {
        if (enabled) ::setenv("REALMHEART_WIFI_HIDE_IN_USE", "1", 1);
        else ::unsetenv("REALMHEART_WIFI_HIDE_IN_USE");
    }

private:
    void write_state(const std::string& state) {
        std::ofstream output(state_file_);
        output << state;
    }

    std::filesystem::path directory_;
    std::filesystem::path state_file_;
    std::filesystem::path active_file_;
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
        require(initial->ssid == "Realm:Net", "nmcli-escaped SSID should be decoded");
        require(initial->signal_percent == 67, "active WiFi signal should be parsed");

        fake.hide_in_use_marker(true);
        const auto fallback = realmheart::services::Wifi::read();
        require(fallback.has_value(), "WiFi fallback state should be readable");
        require(fallback->ssid == "Realm:Net", "connected profile should prevent false disconnect");
        fake.hide_in_use_marker(false);

        const auto networks = realmheart::services::Wifi::scan();
        require(networks.size() == 3, "duplicate BSSIDs should collapse by SSID");
        require(networks.front().ssid == "Realm:Net", "active network should sort first");
        require(networks.front().active, "active network should be marked connected");
        require(networks.front().saved, "known connection profile should be marked saved");
        require(networks.front().connection_uuid == "uuid-realm", "saved UUID should be retained");

        const auto disconnected = realmheart::services::Wifi::disconnect();
        require(disconnected.success, "disconnecting the active WiFi device should succeed");
        require(disconnected.state.ssid.empty(), "disconnect readback should have no SSID");

        const auto saved_connect = realmheart::services::Wifi::connect(
            "Realm:Net", std::nullopt, "uuid-realm"
        );
        require(saved_connect.success, "saved WiFi profile should reconnect by UUID");

        const auto secure_connect = realmheart::services::Wifi::connect(
            "SecretNet", std::string("aether-key")
        );
        require(secure_connect.success, "unknown secured network should accept a password");

        const auto forgotten = realmheart::services::Wifi::forget("Realm:Net", "uuid-realm");
        require(forgotten.success, "saved WiFi profile should be forgettable");

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
