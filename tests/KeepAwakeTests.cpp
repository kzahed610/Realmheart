#include "services/KeepAwake.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class TemporaryFakeInhibit {
public:
    TemporaryFakeInhibit() {
        char pattern[] = "/tmp/realmheart-inhibit-tests-XXXXXX";
        const char* created = ::mkdtemp(pattern);
        if (created == nullptr) throw std::runtime_error("mkdtemp failed");
        directory_ = created;
        state_file_ = directory_ / "inhibit.state";
        std::ofstream(state_file_) << "inactive";

        const auto executable = directory_ / "systemd-inhibit";
        std::ofstream script(executable);
        script << "#!/bin/sh\n"
               << "if [ \"$1\" = --list ]; then\n"
               << "  IFS= read -r state < \"$REALMHEART_INHIBIT_TEST_STATE\"\n"
               << "  [ \"$state\" = active ] && printf 'Realmheart 1000 user 42 sleep idle Keep Awake block\\n'\n"
               << "  exit 0\n"
               << "fi\n"
               << "printf active > \"$REALMHEART_INHIBIT_TEST_STATE\"\n"
               << "trap 'printf inactive > \"$REALMHEART_INHIBIT_TEST_STATE\"; exit 0' TERM INT\n"
               << "while :; do /bin/sleep 1; done\n";
        script.close();
        ::chmod(executable.c_str(), 0700);

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path != nullptr ? old_path : "";
        ::setenv("PATH", directory_.c_str(), 1);
        ::setenv("REALMHEART_INHIBIT_TEST_STATE", state_file_.c_str(), 1);
    }

    ~TemporaryFakeInhibit() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_INHIBIT_TEST_STATE");
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
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
        TemporaryFakeInhibit fake;
        realmheart::services::KeepAwake keep_awake;

        require(!keep_awake.active(), "Keep Awake should start inactive");
        require(keep_awake.set_enabled(true), "enabling inhibitor should pass readback");
        require(keep_awake.active(), "Keep Awake should report active");
        require(keep_awake.set_enabled(false), "disabling inhibitor should pass readback");
        require(!keep_awake.active(), "Keep Awake should report inactive");
    } catch (const std::exception& error) {
        std::cerr << "KeepAwakeTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "KeepAwakeTests passed\n";
    return 0;
}
