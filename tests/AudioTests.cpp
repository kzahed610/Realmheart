#include "services/Audio.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class TemporaryFakeWpctl {
public:
    TemporaryFakeWpctl() {
        char pattern[] = "/tmp/realmheart-audio-tests-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
        state_file_ = directory_ / "volume.state";
        std::ofstream(state_file_) << "0.40";

        const auto executable = directory_ / "wpctl";
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "case \"$1\" in\n"
               << "  get-volume) if [ \"$2\" = '@DEFAULT_AUDIO_SINK@' ] && [ \"$REALMHEART_AUDIO_DEFAULT_MISSING\" = 1 ]; then exit 2; fi; IFS= read -r value < \"$REALMHEART_AUDIO_TEST_STATE\"; printf 'Volume: %s\\n' \"$value\" ;;\n"
               << "  set-volume) if [ \"$2\" = '@DEFAULT_AUDIO_SINK@' ] && [ \"$REALMHEART_AUDIO_DEFAULT_MISSING\" = 1 ]; then exit 2; fi; if [ \"$REALMHEART_AUDIO_IGNORE_WRITES\" != 1 ]; then printf '%s' \"$3\" > \"$REALMHEART_AUDIO_TEST_STATE\"; fi ;;\n"
               << "  status) [ \"$2\" = '-n' ] || exit 64; printf 'Audio\\n  Sinks:\\n   * 51. Built-in Audio Analog Stereo [vol: 0.50]\\n  Sources:\\n' ;;\n"
               << "  *) printf 'unexpected arguments: %s\\n' \"$*\"; exit 64 ;;\n"
               << "esac\n";
        script.close();
        ::chmod(executable.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("REALMHEART_AUDIO_TEST_STATE", state_file_.c_str(), 1);
        ::unsetenv("REALMHEART_AUDIO_IGNORE_WRITES");
        ::unsetenv("REALMHEART_AUDIO_DEFAULT_MISSING");
    }

    ~TemporaryFakeWpctl() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_AUDIO_TEST_STATE");
        ::unsetenv("REALMHEART_AUDIO_IGNORE_WRITES");
        ::unsetenv("REALMHEART_AUDIO_DEFAULT_MISSING");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void ignore_writes() {
        ::setenv("REALMHEART_AUDIO_IGNORE_WRITES", "1", 1);
    }

    void drop_default_sink() {
        ::setenv("REALMHEART_AUDIO_DEFAULT_MISSING", "1", 1);
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path state_file_;
    std::string old_path_;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(double left, double right) {
    return std::abs(left - right) < 0.001;
}

} // namespace

int main() {
    try {
        TemporaryFakeWpctl fake;

        const auto initial = realmheart::services::Audio::read_default_sink();
        require(initial.has_value(), "audio state should be readable");
        require(near(initial->volume, 0.40), "parsed volume should match wpctl output");
        require(!initial->muted, "unmarked audio should not be muted");

        const auto changed = realmheart::services::Audio::set_default_sink_volume(0.65);
        require(changed.success, "volume write should pass matching readback");
        require(near(changed.state.volume, 0.65), "volume readback should match requested value");

        const auto clamped = realmheart::services::Audio::set_default_sink_volume(2.0);
        require(clamped.success, "clamped volume write should succeed");
        require(near(clamped.state.volume, 1.0), "volume should clamp to the 100% ceiling");

        fake.drop_default_sink();
        const auto fallback = realmheart::services::Audio::read_default_sink();
        require(fallback.has_value(), "audio should fall back to an available hardware sink");
        require(near(fallback->volume, 1.0), "fallback sink volume should be readable");

        const auto fallback_changed = realmheart::services::Audio::set_default_sink_volume(0.55);
        require(fallback_changed.success, "fallback hardware sink should remain writable");
        require(near(fallback_changed.state.volume, 0.55), "fallback sink readback should match");

        fake.ignore_writes();
        const auto mismatch = realmheart::services::Audio::set_default_sink_volume(0.20);
        require(!mismatch.success, "volume readback mismatch must fail");
        require(near(mismatch.state.volume, 0.55), "failed mutation should expose actual volume");
    } catch (const std::exception& error) {
        std::cerr << "AudioTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "AudioTests passed\n";
    return 0;
}
