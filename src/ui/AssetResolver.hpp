#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace realmheart::ui {

std::optional<std::filesystem::path> resolve_icon(
    const std::filesystem::path& icon_root,
    std::string_view logical_name
);

// Resolves a path relative to an asset root while preventing traversal outside
// that root. Unlike resolve_icon(), nested paths are allowed.
std::optional<std::filesystem::path> resolve_project_asset(std::string_view relative_path);

// Legacy Fluent icon lookup retained for existing widgets.
std::optional<std::filesystem::path> resolve_project_icon(std::string_view logical_name);

} // namespace realmheart::ui
