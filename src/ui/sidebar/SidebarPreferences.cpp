#include "ui/sidebar/SidebarPreferences.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <string>

namespace realmheart::ui::sidebar {
namespace {

constexpr std::size_t kMaxPreferenceBytes = 4096;
constexpr mode_t kPrivateDirectoryMode = 0700;
constexpr mode_t kPrivateFileMode = 0600;

bool valid_file_name(std::string_view file_name) {
    return !file_name.empty() && file_name.find('/') == std::string_view::npos &&
        file_name != "." && file_name != "..";
}

bool secure_directory(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;

    struct stat metadata{};
    if (::lstat(directory.c_str(), &metadata) != 0 ||
        !S_ISDIR(metadata.st_mode) ||
        metadata.st_uid != ::geteuid()) {
        return false;
    }
    if ((metadata.st_mode & 0077U) != 0U &&
        ::chmod(directory.c_str(), kPrivateDirectoryMode) != 0) {
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> private_directory() {
    std::filesystem::path base;
    if (const char* configured = std::getenv("XDG_CONFIG_HOME");
        configured != nullptr && *configured != '\0') {
        base = configured;
    } else if (const char* home = std::getenv("HOME");
               home != nullptr && *home != '\0') {
        base = std::filesystem::path(home) / ".config";
    } else {
        base = std::filesystem::temp_directory_path() /
            ("realmheart-user-" + std::to_string(static_cast<unsigned long long>(::geteuid())));
        if (!secure_directory(base)) return std::nullopt;
    }

    const auto realmheart_directory = base / "realmheart";
    const auto features_directory = realmheart_directory / "features";
    if (!secure_directory(realmheart_directory) ||
        !secure_directory(features_directory)) {
        return std::nullopt;
    }
    return features_directory;
}

bool inspect_existing_file(
    const std::filesystem::path& path,
    struct stat& metadata,
    bool allow_missing
) {
    if (::lstat(path.c_str(), &metadata) != 0) {
        return allow_missing && errno == ENOENT;
    }
    return S_ISREG(metadata.st_mode) && metadata.st_uid == ::geteuid();
}

bool write_all(int fd, std::string_view value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const ssize_t written = ::write(
            fd,
            value.data() + offset,
            value.size() - offset
        );
        if (written <= 0) {
            if (written < 0 && errno == EINTR) continue;
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

} // namespace

std::optional<std::filesystem::path> sidebar_preference_path(
    std::string_view file_name
) {
    if (!valid_file_name(file_name)) return std::nullopt;
    const auto directory = private_directory();
    if (!directory) return std::nullopt;
    return *directory / std::string(file_name);
}

std::optional<std::string> read_sidebar_preference(std::string_view file_name) {
    const auto path = sidebar_preference_path(file_name);
    if (!path) return std::nullopt;

    struct stat metadata{};
    if (!inspect_existing_file(*path, metadata, true)) return std::nullopt;
    if (metadata.st_size < 0 ||
        static_cast<std::uintmax_t>(metadata.st_size) > kMaxPreferenceBytes) {
        return std::nullopt;
    }

    const int fd = ::open(path->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return std::nullopt;
    if (::fchmod(fd, kPrivateFileMode) != 0) {
        ::close(fd);
        return std::nullopt;
    }

    std::string value;
    value.resize(static_cast<std::size_t>(metadata.st_size));
    std::size_t offset = 0;
    bool success = true;
    while (offset < value.size()) {
        const ssize_t count = ::read(fd, value.data() + offset, value.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            success = false;
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::close(fd) != 0) success = false;
    if (!success) return std::nullopt;
    return value;
}

bool write_sidebar_preference(std::string_view file_name, std::string_view value) {
    if (value.size() > kMaxPreferenceBytes) return false;
    const auto path = sidebar_preference_path(file_name);
    if (!path) return false;

    struct stat existing{};
    if (!inspect_existing_file(*path, existing, true)) return false;

    const std::string stem = path->filename().string() + ".tmp." +
        std::to_string(static_cast<unsigned long long>(::getpid()));
    std::filesystem::path temporary;
    int fd = -1;
    for (unsigned int attempt = 0; attempt < 32; ++attempt) {
        temporary = path->parent_path() /
            (stem + "." + std::to_string(attempt));
        fd = ::open(
            temporary.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            kPrivateFileMode
        );
        if (fd >= 0) break;
        if (errno != EEXIST) return false;
    }
    if (fd < 0) return false;

    bool success = write_all(fd, value);
    if (success && ::fchmod(fd, kPrivateFileMode) != 0) success = false;
    if (success && ::fsync(fd) != 0) success = false;
    if (::close(fd) != 0) success = false;

    if (success) {
        struct stat destination{};
        if (!inspect_existing_file(*path, destination, true)) {
            success = false;
        } else if (::rename(temporary.c_str(), path->c_str()) != 0) {
            success = false;
        }
    }
    if (!success) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
    }
    return success;
}

} // namespace realmheart::ui::sidebar
