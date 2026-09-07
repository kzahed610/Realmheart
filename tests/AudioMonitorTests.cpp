#include "services/AudioMonitor.hpp"

#include <gio/gio.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class FakeAudioTools {
public:
    FakeAudioTools() {
        char pattern[] = "/tmp/realmheart-audio-monitor-tests-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        root_ = created;
        state_file_ = root_ / "volume.state";
        std::ofstream(state_file_) << "0.40\n";

        const auto wpctl = root_ / "wpctl";
        std::ofstream wpctl_script(wpctl);
        wpctl_script << "#!/bin/sh\n"
                     << "if [ \"$1\" = get-volume ]; then IFS= read -r value < \"$REALMHEART_AUDIO_MONITOR_STATE\"; printf 'Volume: %s\\n' \"$value\"; exit 0; fi\n"
                     << "exit 64\n";
        wpctl_script.close();
        ::chmod(wpctl.c_str(), 0700);

        const auto pactl = root_ / "pactl";
        std::ofstream pactl_script(pactl);
        pactl_script << "#!/bin/sh\nexit 7\n";
        pactl_script.close();
        ::chmod(pactl.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", root_.c_str(), 1);
        ::setenv("REALMHEART_AUDIO_MONITOR_STATE", state_file_.c_str(), 1);
    }

    ~FakeAudioTools() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_AUDIO_MONITOR_STATE");
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    void set_volume(const char* value) {
        std::ofstream(state_file_) << value << '\n';
    }

private:
    std::filesystem::path root_;
    std::filesystem::path state_file_;
    std::string old_path_;
};

void iterate_main_context_for(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_pending(nullptr)) {
            g_main_context_iteration(nullptr, FALSE);
        }
        std::this_thread::sleep_for(10ms);
    }
}

} // namespace

int main() {
    try {
        FakeAudioTools tools;
        int callback_count = 0;
        realmheart::services::AudioMonitor monitor(
            [&callback_count](const realmheart::services::AudioState&) {
                ++callback_count;
            }
        );

        monitor.start();
        iterate_main_context_for(400ms);
        tools.set_volume("0.60");
        iterate_main_context_for(700ms);
        require(callback_count > 0, "failed pactl subscription must promote fallback polling");

        monitor.stop();
        callback_count = 0;
        monitor.start();
        iterate_main_context_for(400ms);
        tools.set_volume("0.75");
        iterate_main_context_for(900ms);
        require(callback_count > 0, "AudioMonitor callback must survive stop/start");
        monitor.stop();
    } catch (const std::exception& error) {
        std::cerr << "AudioMonitorTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "AudioMonitor tests passed\n";
    return 0;
}
