#pragma once

#include "events/EventProtocol.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace realmheart::core {

inline constexpr const char* kDoctorEventSource = "realmheart-doctor";

// Strict incident ids keep Event Surface callbacks from becoming an arbitrary
// command launcher.  The Python Doctor remains authoritative for loading the
// incident and deciding what can actually be repaired.
bool valid_doctor_incident_id(std::string_view incident_id);

// Translate one registered Event Surface callback into a terminal command.
// Returns an empty argv when the callback is malformed, does not belong to
// Realmheart Doctor, or the Doctor/terminal executable cannot be resolved.
std::vector<std::string> doctor_event_action_command(
    const realmheart::events::Json& invocation,
    const std::string& executable_dir
);

} // namespace realmheart::core
