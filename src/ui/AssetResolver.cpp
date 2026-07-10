#include "ui/AssetResolver.hpp"

#include <system_error>

#ifndef REALMHEART_ASSET_DIR
#define REALMHEART_ASSET_DIR "assets"
#endif

namespace realmheart::ui {

std::optional<std::filesystem::path> resolve_icon(
    const std::filesystem::path& icon_root,
    std::string_view logical_name
) {
    if (logical_name.empty()) return std::nullopt;

    const std::filesystem::path relative(logical_name);
    if (relative.is_absolute() || relative.has_parent_path() || relative.filename() != relative) {
        return std::nullopt;
    }

    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(icon_root, error);
    if (error) return std::nullopt;

    const auto candidate = std::filesystem::weakly_canonical(canonical_root / relative, error);
    if (error || candidate.parent_path() != canonical_root) return std::nullopt;
    if (!std::filesystem::is_regular_file(candidate, error) || error) return std::nullopt;
    return candidate;
}

std::optional<std::filesystem::path> resolve_project_icon(std::string_view logical_name) {
    return resolve_icon(std::filesystem::path(REALMHEART_ASSET_DIR) / "icons" / "fluent", logical_name);
}

} // namespace realmheart::ui
