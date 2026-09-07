#include "ui/AssetResolver.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
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

std::optional<std::vector<std::string>> normalized_components(
    std::string_view relative_path
) {
    if (relative_path.empty()) return std::nullopt;

    const std::filesystem::path relative(relative_path);
    if (relative.is_absolute()) return std::nullopt;

    std::vector<std::string> components;
    for (const auto& component : relative) {
        if (component == "." || component.empty()) continue;
        if (component == "..") {
            if (components.empty()) return std::nullopt;
            components.pop_back();
            continue;
        }
        components.push_back(component.string());
    }
    if (components.empty()) return std::nullopt;
    return components;
}

int open_directory_chain(const std::filesystem::path& absolute_path) {
    if (!absolute_path.is_absolute()) return -1;

    int current = ::open(
        "/",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
    );
    if (current < 0) return -1;

    for (auto iterator = absolute_path.begin(); iterator != absolute_path.end(); ++iterator) {
        if (*iterator == "/" || *iterator == ".") continue;
        const int next = ::openat(
            current,
            iterator->c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
        );
        if (next < 0) {
            ::close(current);
            return -1;
        }
        ::close(current);
        current = next;
    }
    return current;
}

std::optional<std::pair<int, std::filesystem::path>> open_from_root(
    const std::filesystem::path& canonical_root,
    const std::vector<std::string>& components
) {
    const int root_descriptor = open_directory_chain(canonical_root);
    if (root_descriptor < 0) return std::nullopt;

    int directory = root_descriptor;
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        const int next = ::openat(
            directory,
            components[index].c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW
        );
        if (next < 0) {
            ::close(directory);
            return std::nullopt;
        }
        ::close(directory);
        directory = next;
    }

    const auto candidate = canonical_root /
        [&components] {
            std::filesystem::path result;
            for (const auto& component : components) result /= component;
            return result;
        }();
    const int descriptor = ::openat(
        directory,
        components.back().c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW
    );
    ::close(directory);
    if (descriptor < 0) return std::nullopt;

    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        ::close(descriptor);
        return std::nullopt;
    }
    return std::pair{descriptor, candidate};
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

ProjectAsset::ProjectAsset(int descriptor, std::filesystem::path path) noexcept
    : descriptor_(descriptor), path_(std::move(path)) {}

ProjectAsset::ProjectAsset(ProjectAsset&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      path_(std::move(other.path_)) {}

ProjectAsset& ProjectAsset::operator=(ProjectAsset&& other) noexcept {
    if (this == &other) return *this;
    if (descriptor_ >= 0) ::close(descriptor_);
    descriptor_ = std::exchange(other.descriptor_, -1);
    path_ = std::move(other.path_);
    return *this;
}

ProjectAsset::~ProjectAsset() {
    if (descriptor_ >= 0) ::close(descriptor_);
}

const std::filesystem::path& ProjectAsset::path() const noexcept { return path_; }

int ProjectAsset::descriptor() const noexcept { return descriptor_; }

std::optional<std::string> ProjectAsset::read_all(std::size_t maximum_bytes) const {
    if (descriptor_ < 0) return std::nullopt;

    struct stat status {};
    if (::fstat(descriptor_, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 ||
        static_cast<std::uintmax_t>(status.st_size) > maximum_bytes) {
        return std::nullopt;
    }

    const auto size = static_cast<std::size_t>(status.st_size);
    std::string content(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t result = ::pread(
            descriptor_,
            content.data() + offset,
            size - offset,
            static_cast<off_t>(offset)
        );
        if (result > 0) {
            offset += static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return std::nullopt;
        }
    }
    return content;
}

std::optional<ProjectAsset> open_project_asset(std::string_view relative_path) {
    const auto components = normalized_components(relative_path);
    if (!components) return std::nullopt;

    for (const auto& root : asset_roots()) {
        std::error_code error;
        const auto canonical_root = std::filesystem::weakly_canonical(root, error);
        if (error || !std::filesystem::is_directory(canonical_root, error) || error) {
            continue;
        }
        if (auto opened = open_from_root(canonical_root, *components)) {
            return ProjectAsset(opened->first, std::move(opened->second));
        }
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
