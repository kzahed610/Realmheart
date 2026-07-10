#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace realmheart::ui {

std::optional<std::filesystem::path> resolve_icon(
    const std::filesystem::path& icon_root,
    std::string_view logical_name
);

std::optional<std::filesystem::path> resolve_project_icon(std::string_view logical_name);

} // namespace realmheart::ui
