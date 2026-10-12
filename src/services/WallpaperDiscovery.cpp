#include "services/WallpaperDiscovery.hpp"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>
#include <filesystem>
#include <sys/stat.h>

// Discovery moved here from mana_core::WallpaperLibrary so the shell service
// and the selector share one implementation. mana_core::WallpaperLibrary now
// delegates to this file; its behaviour, and its tests, are unchanged.

namespace realmheart::services {
namespace {

// Mirrors the set the wallpaper service can persist, so anything applicable is
// also selectable. Live videos are discovered but not decoded here: the
// renderer classifies them at apply time.
bool supported_extension(std::string extension) {
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); }
    );

    constexpr std::string_view supported[] = {
        ".png", ".jpg", ".jpeg", ".webp", ".avif", ".bmp", ".tif", ".tiff",
        ".mp4", ".m4v", ".webm", ".mkv", ".mov", ".avi"
    };
    return std::find(
        std::begin(supported),
        std::end(supported),
        extension
    ) != std::end(supported);
}

// Live videos are discovered but never decoded here. Which kind a file is ends
// up decided by the renderer at apply time, so discovery only has to avoid
// treating a valid video as garbage.
bool is_video_extension(std::string extension) {
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); }
    );

    constexpr std::string_view videos[] = {
        ".mp4", ".m4v", ".webm", ".mkv", ".mov", ".avi"
    };
    return std::find(std::begin(videos), std::end(videos), extension) != std::end(videos);
}

std::string lowercase_filename(const std::filesystem::path& path) {
    std::string filename = path.filename().string();
    std::transform(
        filename.begin(),
        filename.end(),
        filename.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); }
    );
    return filename;
}

bool plausible_wallpaper(const std::filesystem::path& path) {
    // Bounded discovery: probe the magic header instead of fully decoding the
    // image. gdk_pixbuf_new_from_file_at_scale() decodes the ENTIRE frame
    // even for a 1x1 scale, so validating every library member that way made
    // startup block for seconds on large collections (1.1 GB / 39 files took
    // ~13.5 s in the field). The header probe rejects the same garbage
    // (text files, random data) in constant time; images that are
    // magic-valid but truncated are surfaced with a decode error later at
    // preload/show time, where the overlay already handles failure cleanly.
    //
    // A video is accepted on extension alone: demuxing every candidate to
    // validate it is exactly the "do not fully decode every video during
    // library startup" trap, and a corrupt video is reported by the renderer
    // when it is actually applied.
    if (is_video_extension(path.extension().string())) return true;

    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0) return false;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    std::array<char, 12> header{};
    stream.read(header.data(), static_cast<std::streamsize>(header.size()));
    const std::streamsize read = stream.gcount();
    if (read < 8) return false;

    const auto starts_with = [&](const char* magic, std::size_t length) {
        return read >= static_cast<std::streamsize>(length) &&
               std::memcmp(header.data(), magic, length) == 0;
    };

    // PNG: \x89PNG\r\n\x1a\n
    if (starts_with("\x89PNG\r\n\x1a\n", 8)) return true;
    // JPEG: \xFF\xD8\xFF
    if (starts_with("\xFF\xD8\xFF", 3)) return true;
    // WebP: RIFF....WEBP
    if (starts_with("RIFF", 4) && read >= 12 &&
        std::memcmp(header.data() + 8, "WEBP", 4) == 0) {
        return true;
    }
    return false;
}

std::string file_identity(const std::filesystem::path& path) {
    struct stat metadata {};
    if (::stat(path.c_str(), &metadata) == 0) {
        return "inode:" + std::to_string(static_cast<unsigned long long>(metadata.st_dev)) +
            ":" + std::to_string(static_cast<unsigned long long>(metadata.st_ino));
    }

    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return "path:" + (error ? path.lexically_normal() : canonical).generic_string();
}

} // namespace

std::filesystem::path default_wallpaper_root() {
    if (const char* home = g_get_home_dir(); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / "Pictures" / "Wallpapers";
    }
    return {};
}

WallpaperDiscovery discover_wallpapers(const std::filesystem::path& requested_root) {
    WallpaperDiscovery result;
    const std::filesystem::path root = requested_root.empty()
        ? default_wallpaper_root()
        : requested_root;
    if (root.empty()) {
        result.diagnostics.emplace_back("wallpaper root could not be resolved");
        return result;
    }

    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) {
        result.diagnostics.emplace_back(
            "wallpaper root is unavailable: " + root.string()
        );
        return result;
    }

    std::filesystem::directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error
    );
    if (error) {
        result.diagnostics.emplace_back(
            "wallpaper root could not be enumerated: " + error.message()
        );
        return result;
    }

    const std::filesystem::directory_iterator end;
    std::vector<std::filesystem::path> candidates;
    while (iterator != end) {
        const std::filesystem::path path = iterator->path();

        std::error_code type_error;
        const bool regular = std::filesystem::is_regular_file(path, type_error);
        if (type_error) {
            result.diagnostics.emplace_back(
                "ignored unreadable entry: " + path.string()
            );
        } else if (regular && supported_extension(path.extension().string())) {
            if (plausible_wallpaper(path)) {
                candidates.push_back(path);
            } else {
                result.diagnostics.emplace_back(
                    "ignored unsupported or corrupt image: " + path.string()
                );
            }
        }

        iterator.increment(error);
        if (error) {
            result.diagnostics.emplace_back(
                "wallpaper enumeration stopped early: " + error.message()
            );
            break;
        }
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const auto& left, const auto& right) {
            const std::string left_folded = lowercase_filename(left);
            const std::string right_folded = lowercase_filename(right);
            if (left_folded != right_folded) return left_folded < right_folded;
            return left.filename().string() < right.filename().string();
        }
    );

    std::unordered_set<std::string> identities;
    identities.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (identities.insert(file_identity(candidate)).second) {
            result.paths.push_back(candidate);
        } else {
            result.diagnostics.emplace_back(
                "ignored duplicate wallpaper alias: " + candidate.string()
            );
        }
    }

    return result;

    return result;
}

} // namespace realmheart::services
