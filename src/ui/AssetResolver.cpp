#include "ui/AssetResolver.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <system_error>
#include <unistd.h>
#include <vector>

#ifndef REALMHEART_INSTALL_ASSET_DIR
#define REALMHEART_INSTALL_ASSET_DIR "share/realmheart/assets"
#endif

#ifndef REALMHEART_SOURCE_ASSET_DIR
#define REALMHEART_SOURCE_ASSET_DIR "assets"
#endif

namespace realmheart::ui {
namespace {

std::filesystem::path executable_directory() {
    std::array<char, 4096> buffer{};
    const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return {};
    return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(length))).parent_path();
}

std::vector<std::filesystem::path> asset_roots() {
    std::vector<std::filesystem::path> roots;
    if (const char* configured = std::getenv("REALMHEART_ASSET_DIR");
        configured != nullptr && *configured != '\0') {
        roots.emplace_back(configured);
    }

    roots.emplace_back(REALMHEART_INSTALL_ASSET_DIR);
    const auto executable = executable_directory();
    if (!executable.empty()) {
        roots.push_back(executable / "../share/realmheart/assets");
        roots.push_back(executable / "assets");
    }
    roots.emplace_back(REALMHEART_SOURCE_ASSET_DIR); // development-tree fallback
    roots.emplace_back("assets");
    return roots;
}

} // namespace

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

std::optional<std::filesystem::path> resolve_project_asset(std::string_view relative_path) {
    if (relative_path.empty()) return std::nullopt;

    const std::filesystem::path relative(relative_path);
    if (relative.is_absolute()) return std::nullopt;

    for (const auto& root : asset_roots()) {
        std::error_code error;
        const auto canonical_root = std::filesystem::weakly_canonical(root, error);
        if (error) continue;

        const auto candidate = std::filesystem::weakly_canonical(canonical_root / relative, error);
        if (error) continue;

        const auto mismatch = std::mismatch(
            canonical_root.begin(), canonical_root.end(), candidate.begin(), candidate.end()
        );
        if (mismatch.first != canonical_root.end()) continue;
        if (!std::filesystem::is_regular_file(candidate, error) || error) continue;
        return candidate;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> resolve_project_icon(std::string_view logical_name) {
    if (logical_name.empty()) return std::nullopt;
    const std::filesystem::path relative(logical_name);
    if (relative.is_absolute() || relative.has_parent_path() || relative.filename() != relative) {
        return std::nullopt;
    }
    const std::string nested = (std::filesystem::path("icons") / "fluent" / relative).generic_string();
    return resolve_project_asset(nested);
}

} // namespace realmheart::ui
