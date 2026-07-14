#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace realmheart::ui::styles {

// Resolves a path beneath Realmheart's dedicated style roots while rejecting
// absolute paths and traversal outside those roots.
std::optional<std::filesystem::path> resolve_style_module(std::string_view relative_path);

// Loads CSS files from Realmheart's style roots and concatenates them in the
// supplied order. Modules are read once by ThemeStyles at startup; palette
// changes reuse the cached text and only replace the colour definitions.
std::string load_css_modules(std::span<const std::string_view> module_paths);

} // namespace realmheart::ui::styles
