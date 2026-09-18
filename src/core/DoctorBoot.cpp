#include "core/DoctorBoot.hpp"

#include "core/Command.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <unistd.h>

namespace realmheart::core {
namespace {

constexpr const char* kBootSwitchVariable = "REALMHEART_DOCTOR_BOOT";
constexpr const char* kBinaryOverrideVariable = "REALMHEART_DOCTOR_BIN";
constexpr const char* kBinaryName = "realmheart-doctor";

std::optional<std::string> environment_value(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return std::string(value);
}

bool executable_file(const std::string& path) {
    if (path.empty() || path.front() != '/') return false;
    if (::access(path.c_str(), X_OK) != 0) return false;
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::string state_directory() {
    const auto state_home = environment_value("XDG_STATE_HOME");
    if (state_home.has_value() && state_home->front() == '/') {
        return *state_home + "/realmheart/doctor";
    }
    const auto home = environment_value("HOME");
    if (home.has_value() && home->front() == '/') {
        return *home + "/.local/state/realmheart/doctor";
    }
    return {};
}

} // namespace

bool doctor_boot_enabled(const char* configured) {
    if (configured == nullptr || *configured == '\0') return true;
    std::string value(configured);
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return !(value == "0" || value == "false" || value == "no" || value == "off");
}

std::optional<std::string> resolve_doctor_executable(const std::string& executable_dir) {
    const auto override_path = environment_value(kBinaryOverrideVariable);
    if (override_path.has_value()) {
        return executable_file(*override_path) ? override_path : std::nullopt;
    }
    if (!executable_dir.empty()) {
        const std::filesystem::path sibling =
            std::filesystem::path(executable_dir) / kBinaryName;
        if (executable_file(sibling.string())) return sibling.string();
    }
    const auto found = find_in_path(kBinaryName);
    if (found.has_value() && executable_file(*found)) return found;
    return std::nullopt;
}

std::vector<std::string> doctor_boot_command(const std::string& executable_dir) {
    if (!doctor_boot_enabled(std::getenv(kBootSwitchVariable))) return {};
    const auto executable = resolve_doctor_executable(executable_dir);
    if (!executable.has_value()) return {};
    const std::string state_dir = state_directory();
    if (state_dir.empty()) return {};
    return {*executable, "boot", "--state-dir", state_dir};
}

bool spawn_doctor_boot(const std::string& executable_dir) {
    const std::vector<std::string> argv = doctor_boot_command(executable_dir);
    if (argv.empty()) return false;
    return run_background(argv);
}

std::string current_executable_directory() {
    std::error_code error;
    const auto target = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error || target.empty()) return {};
    return target.parent_path().string();
}

} // namespace realmheart::core
