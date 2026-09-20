#include "core/DoctorActions.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

using realmheart::events::Json;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct ScratchDirectory {
    std::filesystem::path path;
    ScratchDirectory() {
        path = std::filesystem::temp_directory_path() /
            ("realmheart-doctor-action-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~ScratchDirectory() { std::filesystem::remove_all(path); }
};

std::string write_executable(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path);
    stream << "#!/bin/sh\nexit 0\n";
    stream.close();
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace
    );
    return path.string();
}

void set_environment(const char* name, const char* value) {
    if (value == nullptr) ::unsetenv(name);
    else ::setenv(name, value, 1);
}

Json invocation(std::string action_id, std::string incident = "RH-20260920-001") {
    return Json{
        {"protocol", 1},
        {"type", "ACTION_INVOKED"},
        {"source_id", "realmheart-doctor"},
        {"event_id", "realmheart-doctor-" + incident},
        {"action_id", std::move(action_id)},
    };
}

void test_incident_validation() {
    require(realmheart::core::valid_doctor_incident_id("RH-20260920-001"),
            "canonical incident id must be accepted");
    require(realmheart::core::valid_doctor_incident_id("RH-20260920-1000"),
            "incident sequence may grow past three digits");
    require(!realmheart::core::valid_doctor_incident_id("../../etc/passwd"),
            "path-like ids must be rejected");
    require(!realmheart::core::valid_doctor_incident_id("RH-2026-001"),
            "truncated ids must be rejected");
}

void test_inspect_and_repair_commands() {
    ScratchDirectory scratch;
    const std::string doctor = write_executable(scratch.path / "bin" / "realmheart-doctor");
    const std::string kitty = write_executable(scratch.path / "bin" / "kitty");
    set_environment("REALMHEART_DOCTOR_BIN", doctor.c_str());
    set_environment("REALMHEART_DOCTOR_TERMINAL", kitty.c_str());

    const auto inspect = realmheart::core::doctor_event_action_command(
        invocation("inspect:RH-20260920-001"), scratch.path.string()
    );
    require(inspect.size() == 8, "inspect action must produce the bounded terminal argv");
    require(inspect[0] == kitty && inspect[4] == doctor,
            "inspect action must use resolved terminal and Doctor binaries");
    require(inspect[5] == "incident" && inspect[6] == "RH-20260920-001" && inspect[7] == "--preview",
            "inspect action must open the saved incident without a shell");

    const auto repair = realmheart::core::doctor_event_action_command(
        invocation("repair:RH-20260920-001"), scratch.path.string()
    );
    require(repair.size() == 9, "repair action must produce the consent-gated terminal argv");
    require(repair[5] == "repair-incident" && repair[6] == "RH-20260920-001",
            "repair action must stay bound to the incident identity");
    require(repair[7] == "--apply" && repair[8] == "--allow-privileged",
            "repair action may enter privileged planning but must leave consent to Doctor");

    set_environment("REALMHEART_DOCTOR_BIN", nullptr);
    set_environment("REALMHEART_DOCTOR_TERMINAL", nullptr);
}

void test_malformed_or_cross_source_callbacks_are_rejected() {
    ScratchDirectory scratch;
    const std::string doctor = write_executable(scratch.path / "realmheart-doctor");
    const std::string kitty = write_executable(scratch.path / "kitty");
    set_environment("REALMHEART_DOCTOR_BIN", doctor.c_str());
    set_environment("REALMHEART_DOCTOR_TERMINAL", kitty.c_str());

    auto wrong_source = invocation("inspect:RH-20260920-001");
    wrong_source["source_id"] = "other-service";
    require(realmheart::core::doctor_event_action_command(wrong_source, scratch.path.string()).empty(),
            "callbacks from another source must be ignored");

    auto mismatched_event = invocation("repair:RH-20260920-001");
    mismatched_event["event_id"] = "realmheart-doctor-RH-20260920-999";
    require(realmheart::core::doctor_event_action_command(mismatched_event, scratch.path.string()).empty(),
            "action incident must match the event identity");

    require(realmheart::core::doctor_event_action_command(
                invocation("repair:../../oops"), scratch.path.string()).empty(),
            "action ids cannot smuggle path content into argv");
    require(realmheart::core::doctor_event_action_command(
                invocation("unknown:RH-20260920-001"), scratch.path.string()).empty(),
            "unknown Doctor actions must be ignored");

    set_environment("REALMHEART_DOCTOR_BIN", nullptr);
    set_environment("REALMHEART_DOCTOR_TERMINAL", nullptr);
}

void test_missing_terminal_disables_registered_action_execution() {
    ScratchDirectory scratch;
    const std::string doctor = write_executable(scratch.path / "realmheart-doctor");
    set_environment("REALMHEART_DOCTOR_BIN", doctor.c_str());
    set_environment("REALMHEART_DOCTOR_TERMINAL", "/definitely/not/a/terminal");
    require(realmheart::core::doctor_event_action_command(
                invocation("inspect:RH-20260920-001"), scratch.path.string()).empty(),
            "missing terminal must fail closed; copy action remains the UI fallback");
    set_environment("REALMHEART_DOCTOR_BIN", nullptr);
    set_environment("REALMHEART_DOCTOR_TERMINAL", nullptr);
}

} // namespace

int main() {
    test_incident_validation();
    test_inspect_and_repair_commands();
    test_malformed_or_cross_source_callbacks_are_rejected();
    test_missing_terminal_disables_registered_action_execution();
    std::cout << "Realmheart Doctor Event Surface action tests passed\n";
    return 0;
}
