#pragma once

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace realmheart::ui {

std::optional<std::filesystem::path> resolve_icon(
    const std::filesystem::path& icon_root,
    std::string_view logical_name
);

// Resolves a path relative to an asset root while preventing traversal outside
// that root. This path-returning API is for metadata and diagnostics only;
// consumers that open asset bytes must use open_project_asset().
std::optional<std::filesystem::path> resolve_project_asset(std::string_view relative_path);

class ProjectAsset {
public:
    ProjectAsset(const ProjectAsset&) = delete;
    ProjectAsset& operator=(const ProjectAsset&) = delete;
    ProjectAsset(ProjectAsset&& other) noexcept;
    ProjectAsset& operator=(ProjectAsset&& other) noexcept;
    ~ProjectAsset();

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] int descriptor() const noexcept;
    [[nodiscard]] std::optional<std::string> read_all(
        std::size_t maximum_bytes
    ) const;

private:
    friend std::optional<ProjectAsset> open_project_asset(std::string_view);
    ProjectAsset(int descriptor, std::filesystem::path path) noexcept;

    int descriptor_ = -1;
    std::filesystem::path path_;
};

// Opens a regular project asset through a no-follow descriptor rooted at the
// selected asset root. The returned handle owns the descriptor used by
// read_all().
[[nodiscard]] std::optional<ProjectAsset> open_project_asset(
    std::string_view relative_path
);

// Legacy Fluent icon lookup retained for metadata/diagnostic callers.
std::optional<std::filesystem::path> resolve_project_icon(std::string_view logical_name);

} // namespace realmheart::ui
