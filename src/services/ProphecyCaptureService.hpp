#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace realmheart::services {

// Manages workspace screenshot capture for the Prophecy lock screen.
// Screenshots are captured while a workspace is active (before switching away)
// and stored in $XDG_RUNTIME_DIR/realmheart/prophecy/.
//
// At lock time, the renderer loads these stored screenshots as prophecy shards —
// no capture needed during the lock sequence (zero latency).
class ProphecyCaptureService {
public:
    struct WorkspaceScreenshot {
        int workspace_id = 0;
        std::filesystem::path path;
    };

    // Get/create the prophecy screenshot directory.
    // Returns empty path on failure.
    static std::filesystem::path ensure_prophecy_dir();

    // Capture the currently active workspace screenshot.
    // Runs grim to capture the full output, saves to prophecy dir.
    // Returns the path to the saved screenshot, or empty on failure.
    static std::filesystem::path capture_active_workspace();

    // Capture a specific workspace by ID (for targeted captures).
    // Note: grim captures the current output, so this only works
    // if the workspace is currently visible.
    static std::filesystem::path capture_workspace(int workspace_id);

    // List all stored workspace screenshots.
    static std::vector<WorkspaceScreenshot> list_screenshots();

    // Clean up old screenshots (e.g., on session start or unlock).
    static void cleanup();
};

} // namespace realmheart::services
