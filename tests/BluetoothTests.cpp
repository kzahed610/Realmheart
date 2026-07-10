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
        std::ofstream(state_file_) << "yes";

        const auto executable = directory_ / "bluetoothctl";
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "case \"$*\" in\n"
               << "  show) IFS= read -r state < \"$REALMHEART_BLUETOOTH_TEST_STATE\"; printf 'Controller 00:11:22:33:44:55\\n\\tPowered: %s\\n' \"$state\" ;;\n"
               << "  'power on') if [ \"$REALMHEART_BLUETOOTH_IGNORE_WRITES\" != 1 ]; then printf yes > \"$REALMHEART_BLUETOOTH_TEST_STATE\"; fi ;;\n"
               << "  'power off') if [ \"$REALMHEART_BLUETOOTH_IGNORE_WRITES\" != 1 ]; then printf no > \"$REALMHEART_BLUETOOTH_TEST_STATE\"; fi ;;\n"
               << "  *) printf 'unexpected arguments: %s\\n' \"$*\"; exit 64 ;;\n"
               << "esac\n";
        script.close();
        ::chmod(executable.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("REALMHEART_BLUETOOTH_TEST_STATE", state_file_.c_str(), 1);
        ::unsetenv("REALMHEART_BLUETOOTH_IGNORE_WRITES");
    }

    ~TemporaryFakeBluetoothctl() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_BLUETOOTH_TEST_STATE");
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
