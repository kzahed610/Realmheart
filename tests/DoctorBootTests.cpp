#include "core/DoctorBoot.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void set_environment(const char* name, const char* value) {
    if (value == nullptr) {
        ::unsetenv(name);
        return;
    }
    ::setenv(name, value, 1);
}

struct ScratchDirectory {
    std::filesystem::path path;

    ScratchDirectory() {
        path = std::filesystem::temp_directory_path() /
            ("realmheart-doctor-boot-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(path);
    }

    ~ScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string write_executable(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "#!/bin/true\n";
    ::chmod(path.c_str(), 0755);
    return path.string();
}

void test_kill_switch_parsing() {
    require(realmheart::core::doctor_boot_enabled(nullptr), "unset kill switch must stay enabled");
    require(realmheart::core::doctor_boot_enabled(""), "empty kill switch must stay enabled");
    require(realmheart::core::doctor_boot_enabled("1"), "explicit 1 must stay enabled");
    require(!realmheart::core::doctor_boot_enabled("0"), "0 must disable the boot one-shot");
    require(!realmheart::core::doctor_boot_enabled("NO"), "case-insensitive no must disable");
    require(!realmheart::core::doctor_boot_enabled("off"), "off must disable");
}

void test_env_override_builds_the_boot_command() {
    ScratchDirectory scratch;
    const std::string executable = write_executable(scratch.path / "bin" / "realmheart-doctor");
    set_environment("REALMHEART_DOCTOR_BIN", executable.c_str());
    set_environment("REALMHEART_DOCTOR_BOOT", nullptr);
    set_environment("XDG_STATE_HOME", (scratch.path / "state").c_str());

    const auto argv = realmheart::core::doctor_boot_command("");
    require(argv.size() == 4, "boot command must carry executable, subcommand and state directory");
    require(argv[0] == executable, "override executable must be used");
    require(argv[1] == "boot", "boot subcommand must be explicit");
    require(argv[2] == "--state-dir", "state directory flag must be explicit");
    require(argv[3] == (scratch.path / "state" / "realmheart" / "doctor").string(),
            "state directory must follow XDG_STATE_HOME");
    set_environment("REALMHEART_DOCTOR_BIN", nullptr);
    set_environment("XDG_STATE_HOME", nullptr);
}

void test_sibling_and_disabled_paths() {
    ScratchDirectory scratch;
    set_environment("REALMHEART_DOCTOR_BIN", nullptr);
    set_environment("XDG_STATE_HOME", (scratch.path / "state").c_str());

    const char* inherited_path = std::getenv("PATH");
    const bool had_path = inherited_path != nullptr;
    const std::string saved_path = had_path ? inherited_path : "";
    const std::string isolated_path = (scratch.path / "isolated-path").string();
    set_environment("PATH", isolated_path.c_str());

    const auto missing = realmheart::core::doctor_boot_command(scratch.path.string());
    require(missing.empty(), "an unresolvable executable must produce no command");

    const std::string sibling = write_executable(scratch.path / "realmheart-doctor");
    const auto resolved = realmheart::core::doctor_boot_command(scratch.path.string());
    require(resolved.size() == 4 && resolved[0] == sibling, "sibling executable must be resolved");

    set_environment("REALMHEART_DOCTOR_BOOT", "0");
    require(realmheart::core::doctor_boot_command(scratch.path.string()).empty(),
            "kill switch must suppress the command");
    set_environment("REALMHEART_DOCTOR_BOOT", nullptr);
    set_environment("XDG_STATE_HOME", nullptr);
    set_environment("PATH", had_path ? saved_path.c_str() : nullptr);
}

void test_path_fallback_builds_the_boot_command() {
    ScratchDirectory scratch;
    set_environment("REALMHEART_DOCTOR_BIN", nullptr);
    set_environment("REALMHEART_DOCTOR_BOOT", nullptr);
    set_environment("XDG_STATE_HOME", (scratch.path / "state").c_str());

    const char* inherited_path = std::getenv("PATH");
    const bool had_path = inherited_path != nullptr;
    const std::string saved_path = had_path ? inherited_path : "";
    const auto path_bin = scratch.path / "path-bin";
    const std::string executable = write_executable(path_bin / "realmheart-doctor");
    const std::string isolated_path = path_bin.string();
    set_environment("PATH", isolated_path.c_str());

    const auto argv = realmheart::core::doctor_boot_command((scratch.path / "no-sibling").string());
    require(argv.size() == 4 && argv[0] == executable,
            "PATH fallback executable must be resolved when no sibling exists");

    set_environment("PATH", had_path ? saved_path.c_str() : nullptr);
    set_environment("XDG_STATE_HOME", nullptr);
}

void test_missing_state_home_suppresses_the_spawn() {
    ScratchDirectory scratch;
    const std::string executable = write_executable(scratch.path / "realmheart-doctor");
    set_environment("REALMHEART_DOCTOR_BIN", executable.c_str());
    set_environment("XDG_STATE_HOME", nullptr);
    set_environment("HOME", nullptr);
    require(realmheart::core::doctor_boot_command(scratch.path.string()).empty(),
            "an unknown state directory must suppress the boot command");
    set_environment("REALMHEART_DOCTOR_BIN", nullptr);
}

void test_spawn_is_best_effort_and_detached() {
    ScratchDirectory scratch;
    set_environment("REALMHEART_DOCTOR_BIN", nullptr);
    set_environment("REALMHEART_DOCTOR_BOOT", "0");
    require(!realmheart::core::spawn_doctor_boot(scratch.path.string()),
            "a disabled boot must not report a spawn");
    set_environment("REALMHEART_DOCTOR_BOOT", nullptr);
}

} // namespace

int main() {
    test_kill_switch_parsing();
    test_env_override_builds_the_boot_command();
    test_sibling_and_disabled_paths();
    test_path_fallback_builds_the_boot_command();
    test_missing_state_home_suppresses_the_spawn();
    test_spawn_is_best_effort_and_detached();
    std::cout << "realmheart doctor boot tests passed\n";
    return 0;
}
