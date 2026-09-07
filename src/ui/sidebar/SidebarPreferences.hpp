#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace realmheart::ui::sidebar {

// Resolves a preference below Realmheart's private per-user configuration
// directory. The directory is created with owner-only permissions when needed.
[[nodiscard]] std::optional<std::filesystem::path> sidebar_preference_path(
    std::string_view file_name
);

// Reads a small preference without following a symlink or accepting an
// unexpectedly large file. Missing or unsafe files are treated as unavailable.
[[nodiscard]] std::optional<std::string> read_sidebar_preference(
    std::string_view file_name
);

// Publishes a preference through an exclusive 0600 temporary file and an
// atomic rename. Returns false for unsafe paths, bounded I/O failures, or
// directory/permission failures.
[[nodiscard]] bool write_sidebar_preference(
    std::string_view file_name,
    std::string_view value
);

} // namespace realmheart::ui::sidebar
