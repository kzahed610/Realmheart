#pragma once

#include "core/Command.hpp"

#include <nlohmann_json/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realmheart::services {

// One restorable application, captured from a live Hyprland client plus that
// client's process command line.
//
// Capture is deliberately narrow: class, workspace, working directory and the
// exact argv. Window geometry, tiling state and focus order are not kept,
// because restoring those faithfully is a compositor problem. This is a
// convenience layer, not a session manager.
struct SessionRestoreEntry {
    std::string app_class;
    std::string title;
    int workspace_id = 0;
    std::string working_directory;
    std::vector<std::string> argv;

    // True when this entry could actually be relaunched as-is.
    [[nodiscard]] bool restorable() const;
};

// A captured set of applications, tagged with the boot they came from. The
// boot id is what makes "offer to restore" honest: it only fires when the
// snapshot on disk was written by a previous boot.
struct SessionRestoreSnapshot {
    static constexpr int kCurrentSchemaVersion = 1;

    bool available = false;
    std::string error;
    std::string boot_id;
    std::int64_t captured_at_epoch = 0;
    std::vector<SessionRestoreEntry> entries;

    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] static SessionRestoreSnapshot from_json(const nlohmann::json& document);
};

class SessionRestoreStore {
public:
    static constexpr int kCurrentSchemaVersion = SessionRestoreSnapshot::kCurrentSchemaVersion;

    explicit SessionRestoreStore(std::filesystem::path path = {});

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] static std::filesystem::path default_path();

    // Missing file is not an error: it simply means "nothing to restore".
    [[nodiscard]] std::optional<SessionRestoreSnapshot> load(
        std::string* diagnostic = nullptr
    ) const;
    bool save(
        const SessionRestoreSnapshot& snapshot,
        std::string* diagnostic = nullptr
    ) const;
    bool clear(std::string* diagnostic = nullptr) const;

private:
    std::filesystem::path path_;
};

// Reads /proc/sys/kernel/random/boot_id. Empty when unavailable.
[[nodiscard]] std::string current_boot_id();

// True when a stored snapshot deserves the "pick up where you left off" prompt:
// it has entries and it came from a different boot than this one.
[[nodiscard]] bool is_pending_restore(
    const SessionRestoreSnapshot& snapshot,
    std::string_view boot_id
);

// Capture the live session. Unavailable when hyprctl is missing or the client
// list cannot be read; entries are empty rather than guessed.
[[nodiscard]] SessionRestoreSnapshot capture_session(
    const core::CommandOptions& options = {}
);

// Pure: drop entries that cannot be relaunched and collapse duplicates so each
// application appears once.
[[nodiscard]] std::vector<SessionRestoreEntry> restorable_entries(
    std::vector<SessionRestoreEntry> entries
);

// Pure: the shell command that relaunches one entry in its old directory.
[[nodiscard]] std::string build_launch_command(const SessionRestoreEntry& entry);

// Relaunch the given entries on their original workspaces. Returns how many
// applications Hyprland accepted.
[[nodiscard]] std::size_t restore_entries(
    const std::vector<SessionRestoreEntry>& entries,
    const core::CommandOptions& options = {}
);

} // namespace realmheart::services