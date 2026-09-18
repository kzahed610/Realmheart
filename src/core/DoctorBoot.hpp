#pragma once

#include <optional>
#include <string>
#include <vector>

namespace realmheart::core {

// Automatic Doctor boots are spawned once per shell session.  The Python
// boundary keeps its own session dedupe; this layer only resolves, plans and
// launches, and never blocks or fails shell startup.

// Parse the REALMHEART_DOCTOR_BOOT kill switch ("0", "false", "no", "off").
bool doctor_boot_enabled(const char* configured);

// Resolve realmheart-doctor: REALMHEART_DOCTOR_BIN, then a sibling of the
// shell executable, then PATH.  Only executable regular files are returned.
std::optional<std::string> resolve_doctor_executable(const std::string& executable_dir);

// Full argv for the boot one-shot; empty when disabled, unresolved, or the
// state directory cannot be determined.
std::vector<std::string> doctor_boot_command(const std::string& executable_dir);

// Detached, best-effort spawn of the boot one-shot.  Returns false when
// nothing was started; startup is never affected either way.
bool spawn_doctor_boot(const std::string& executable_dir);

// Directory of the running executable (/proc/self/exe), empty on failure.
std::string current_executable_directory();

} // namespace realmheart::core
