#include "core/DoctorActions.hpp"

#include "core/Command.hpp"
#include "core/DoctorBoot.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>
#include <unistd.h>

namespace realmheart::core {
namespace {

constexpr const char* kTerminalOverrideVariable = "REALMHEART_DOCTOR_TERMINAL";
constexpr const char* kDefaultTerminal = "kitty";
constexpr std::string_view kInspectPrefix = "inspect:";
constexpr std::string_view kRepairPrefix = "repair:";
constexpr std::string_view kEventPrefix = "realmheart-doctor-";

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

std::optional<std::string> resolve_terminal() {
    if (const auto override_path = environment_value(kTerminalOverrideVariable)) {
        return executable_file(*override_path) ? override_path : std::nullopt;
    }
    const auto found = find_in_path(kDefaultTerminal);
    if (found.has_value() && executable_file(*found)) return found;
    return std::nullopt;
}

std::optional<std::pair<std::string_view, std::string_view>> parse_action(
    std::string_view action_id
) {
    if (action_id.starts_with(kInspectPrefix)) {
        return std::pair{kInspectPrefix, action_id.substr(kInspectPrefix.size())};
    }
    if (action_id.starts_with(kRepairPrefix)) {
        return std::pair{kRepairPrefix, action_id.substr(kRepairPrefix.size())};
    }
    return std::nullopt;
}

} // namespace

bool valid_doctor_incident_id(std::string_view incident_id) {
    // RH-YYYYMMDD-NNN (sequence may grow beyond three digits).
    if (incident_id.size() < 15 || !incident_id.starts_with("RH-")) return false;
    for (std::size_t index = 3; index < 11; ++index) {
        if (!std::isdigit(static_cast<unsigned char>(incident_id[index]))) return false;
    }
    if (incident_id[11] != '-') return false;
    if (incident_id.size() < 15) return false;
    for (std::size_t index = 12; index < incident_id.size(); ++index) {
        if (!std::isdigit(static_cast<unsigned char>(incident_id[index]))) return false;
    }
    return incident_id.size() >= 15;
}

std::vector<std::string> doctor_event_action_command(
    const realmheart::events::Json& invocation,
    const std::string& executable_dir
) {
    if (!invocation.is_object() || invocation.value("type", "") != "ACTION_INVOKED" ||
        invocation.value("source_id", "") != kDoctorEventSource) {
        return {};
    }
    if (!invocation.contains("event_id") || !invocation["event_id"].is_string() ||
        !invocation.contains("action_id") || !invocation["action_id"].is_string()) {
        return {};
    }

    const std::string event_id = invocation["event_id"].get<std::string>();
    const std::string action_id = invocation["action_id"].get<std::string>();
    const auto parsed = parse_action(action_id);
    if (!parsed.has_value()) return {};
    const std::string incident_id(parsed->second);
    if (!valid_doctor_incident_id(incident_id)) return {};
    if (event_id != std::string(kEventPrefix) + incident_id) return {};

    const auto doctor = resolve_doctor_executable(executable_dir);
    const auto terminal = resolve_terminal();
    if (!doctor.has_value() || !terminal.has_value()) return {};

    std::vector<std::string> argv{
        *terminal,
        "--title", "Realmheart Doctor",
        "--hold",
        *doctor,
    };
    if (parsed->first == kInspectPrefix) {
        argv.insert(argv.end(), {"incident", incident_id, "--preview"});
    } else {
        argv.insert(argv.end(), {
            "repair-incident", incident_id,
            "--apply",
            "--allow-privileged",
        });
    }
    return argv;
}

} // namespace realmheart::core
