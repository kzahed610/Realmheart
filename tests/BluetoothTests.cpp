#include "services/Bluetooth.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class TemporaryFakeBluetoothctl {
public:
    TemporaryFakeBluetoothctl() {
        char pattern[] = "/tmp/realmheart-bluetooth-tests-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
        state_file_ = directory_ / "bluetooth.state";
        connected_file_ = directory_ / "bluetooth.connected";
        std::ofstream(state_file_) << "yes";
        std::ofstream(connected_file_) << "no";

        const auto executable = directory_ / "bluetoothctl";
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "case \"$*\" in\n"
               << "  show) IFS= read -r state < \"$REALMHEART_BLUETOOTH_TEST_STATE\"; printf 'Controller 00:11:22:33:44:55\\n\\tPowered: %s\\n' \"$state\" ;;\n"
               << "  'power on') if [ \"$REALMHEART_BLUETOOTH_IGNORE_WRITES\" != 1 ]; then printf yes > \"$REALMHEART_BLUETOOTH_TEST_STATE\"; fi ;;\n"
               << "  'power off') if [ \"$REALMHEART_BLUETOOTH_IGNORE_WRITES\" != 1 ]; then printf no > \"$REALMHEART_BLUETOOTH_TEST_STATE\"; fi ;;\n"
               << "  '--timeout 5 scan on') printf 'Discovery started\\n' ;;\n"
               << "  devices) printf 'Device AA:BB:CC:DD:EE:01 Aether Headset\\nDevice AA:BB:CC:DD:EE:02 Sylvie Speaker\\n' ;;\n"
               << "  'info AA:BB:CC:DD:EE:01') IFS= read -r connected < \"$REALMHEART_BLUETOOTH_TEST_CONNECTED\"; printf 'Device AA:BB:CC:DD:EE:01\\n\\tName: Aether Headset\\n\\tPaired: yes\\n\\tTrusted: yes\\n\\tConnected: %s\\n' \"$connected\" ;;\n"
               << "  'info AA:BB:CC:DD:EE:02') printf 'Device AA:BB:CC:DD:EE:02\\n\\tName: Sylvie Speaker\\n\\tPaired: no\\n\\tTrusted: no\\n\\tConnected: no\\n' ;;\n"
               << "  'connect AA:BB:CC:DD:EE:01') printf yes > \"$REALMHEART_BLUETOOTH_TEST_CONNECTED\" ;;\n"
               << "  'disconnect AA:BB:CC:DD:EE:01') printf no > \"$REALMHEART_BLUETOOTH_TEST_CONNECTED\" ;;\n"
               << "  'remove AA:BB:CC:DD:EE:01') : ;;\n"
               << "  *) printf 'unexpected arguments: %s\\n' \"$*\"; exit 64 ;;\n"
               << "esac\n";
        script.close();
        ::chmod(executable.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("REALMHEART_BLUETOOTH_TEST_STATE", state_file_.c_str(), 1);
        ::setenv("REALMHEART_BLUETOOTH_TEST_CONNECTED", connected_file_.c_str(), 1);
        ::unsetenv("REALMHEART_BLUETOOTH_IGNORE_WRITES");
    }

    ~TemporaryFakeBluetoothctl() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_BLUETOOTH_TEST_STATE");
        ::unsetenv("REALMHEART_BLUETOOTH_TEST_CONNECTED");
        ::unsetenv("REALMHEART_BLUETOOTH_IGNORE_WRITES");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void ignore_writes() {
        ::setenv("REALMHEART_BLUETOOTH_IGNORE_WRITES", "1", 1);
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path state_file_;
    std::filesystem::path connected_file_;
    std::string old_path_;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        TemporaryFakeBluetoothctl fake;

        const auto initial = realmheart::services::Bluetooth::read();
        require(initial.has_value(), "Bluetooth state should be readable");
        require(initial->powered, "Bluetooth should start powered");

        const auto devices = realmheart::services::Bluetooth::devices(true);
        require(devices.size() == 2, "Bluetooth scan should return discovered devices");
        require(devices.front().name == "Aether Headset", "paired device should sort first");
        require(devices.front().paired, "paired device state should be parsed");

        const auto connected = realmheart::services::Bluetooth::connect(
            "AA:BB:CC:DD:EE:01"
        );
        require(connected.success, "paired Bluetooth device should connect");
        require(connected.device && connected.device->connected, "connect readback should be connected");

        const auto disconnected = realmheart::services::Bluetooth::disconnect(
            "AA:BB:CC:DD:EE:01"
        );
        require(disconnected.success, "Bluetooth device should disconnect");

        const auto forgotten = realmheart::services::Bluetooth::forget(
            "AA:BB:CC:DD:EE:01"
        );
        require(forgotten.success, "paired Bluetooth device should be forgettable");

        const auto disabled = realmheart::services::Bluetooth::set_powered(false);
        require(disabled.success, "Bluetooth power-off should pass matching readback");
        require(!disabled.state.powered, "Bluetooth readback should report powered off");

        const auto enabled = realmheart::services::Bluetooth::set_powered(true);
        require(enabled.success, "Bluetooth power-on should pass matching readback");
        require(enabled.state.powered, "Bluetooth readback should report powered on");

        fake.ignore_writes();
        const auto mismatch = realmheart::services::Bluetooth::set_powered(false);
        require(!mismatch.success, "Bluetooth readback mismatch must fail");
        require(mismatch.state.powered, "failed mutation should expose actual powered state");
    } catch (const std::exception& error) {
        std::cerr << "BluetoothTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "BluetoothTests passed\n";
    return 0;
}
