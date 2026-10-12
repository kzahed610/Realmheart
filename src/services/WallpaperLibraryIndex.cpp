#include "services/WallpaperLibraryIndex.hpp"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <sys/stat.h>

namespace realmheart::services {
namespace {

constexpr std::string_view kCacheRoot =
    ".local/share/realmheart/wallpaper-cache";

// The extension list lives in WallpaperDiscovery, which is what actually walks
// the roots. This one is only about *which* kind a discovered file is.
bool is_video_extension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        }
    );
    constexpr std::array<std::string_view, 6> videos{
        ".mp4", ".m4v", ".webm", ".mkv", ".mov", ".avi"
    };
    return std::find(videos.begin(), videos.end(), extension) != videos.end();
}

// A still image, decided by its header. This is the same cheap probe the
// selector has always used: it never decodes the image, and it never touches a
// video (a live wallpaper is classified from its extension and nothing else,
// because demuxing every candidate is exactly the trap the renderer plan warns
// about).
bool has_image_magic(const std::filesystem::path& path) {
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
    if (starts_with("\x89PNG\r\n\x1a\n", 8)) return true;
    if (starts_with("\xFF\xD8\xFF", 3)) return true;
    return starts_with("RIFF", 4) && read >= 12 &&
           std::memcmp(header.data() + 8, "WEBP", 4) == 0;
}

// size + mtime is the identity the plan asks for, and the inode is what catches
// a file replaced at the same path: a new inode with the same size and a coarse
// mtime is otherwise indistinguishable.
std::uint64_t fingerprint_of(
    const std::filesystem::path& path,
    std::uint64_t size,
    std::int64_t mtime
) {
    std::uint64_t identity = (size << 1) ^ static_cast<std::uint64_t>(mtime);
    struct stat metadata {};
    if (::stat(path.c_str(), &metadata) == 0) {
        identity ^= static_cast<std::uint64_t>(metadata.st_ino) << 17;
        identity ^= static_cast<std::uint64_t>(metadata.st_dev);
    }
    return identity;
}

// True when 'path' lives inside 'root'. The comparison is on the normalised
// path, so a trailing separator or a "." component cannot mislead it.
bool is_inside(const std::filesystem::path& path, const std::filesystem::path& root) {
    if (root.empty()) return false;
    std::error_code error;
    const auto normalized = path.lexically_normal();
    auto relative = std::filesystem::relative(normalized, root, error);
    if (error) return false;
    const auto value = relative.generic_string();
    return !value.empty() && value.rfind("..", 0) != 0;
}

} // namespace

WallpaperLibraryIndex::WallpaperLibraryIndex(WallpaperLibraryRoots roots)
    : roots_(std::move(roots)) {}

std::filesystem::path WallpaperLibraryRoots::effective_static_root() const {
    if (!static_root.empty()) return static_root;
    if (const char* configured = std::getenv("REALMHEART_WALLPAPER_STATIC_ROOT");
        configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured);
    }
    return default_wallpaper_root();
}

std::filesystem::path WallpaperLibraryRoots::effective_live_root() const {
    if (!live_root.empty()) return live_root;
    if (const char* configured = std::getenv("REALMHEART_WALLPAPER_LIVE_ROOT");
        configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured);
    }
    return effective_static_root() / "Live";
}

WallpaperMediaKind WallpaperLibraryIndex::classify(
    const std::filesystem::path& path
) const {
    // Cheap by construction, and never a decode: an image is recognised by its
    // magic bytes, a live wallpaper by its extension. Anything else is a live
    // candidate, which is what the renderer is for.
    if (is_video_extension(path)) return WallpaperMediaKind::Live;
    if (has_image_magic(path)) return WallpaperMediaKind::Static;
    return WallpaperMediaKind::Unknown;
}

WallpaperLibrarySnapshot WallpaperLibraryIndex::rescan() {
    diagnostics_.clear();
    entries_.clear();

    const auto started = std::chrono::steady_clock::now();

    struct Root {
        std::filesystem::path path;
        WallpaperOrigin origin;
        WallpaperMediaKind kind;
    };
    // The live root is listed after the static root so a file appearing in both
    // is reported once, from the static root.
    std::vector<Root> roots;
    const auto static_root = roots_.effective_static_root();
    const auto live_root = roots_.effective_live_root();
    if (!static_root.empty()) {
        roots.push_back(Root{static_root, WallpaperOrigin::Local, WallpaperMediaKind::Static});
    }
    if (!live_root.empty() && live_root != static_root) {
        roots.push_back(Root{live_root, WallpaperOrigin::Local, WallpaperMediaKind::Unknown});
    }

    std::vector<std::filesystem::path> seen;
    for (const auto& root : roots) {
        const auto discovery = discover_wallpapers(root.path);
        for (const auto& message : discovery.diagnostics) {
            diagnostics_.push_back(message);
        }
        if (discovery.paths.empty()) {
            // A root that exists but holds no wallpaper is worth saying; a root
            // discovery already complained about is not.
            if (std::find(diagnostics_.begin(), diagnostics_.end(), "wallpaper root is unavailable: " + root.path.string()) ==
                diagnostics_.end()) {
                diagnostics_.push_back(
                    "wallpaper root holds no wallpapers: " + root.path.string()
                );
            }
            continue;
        }

        for (const auto& path : discovery.paths) {
            if (entries_.size() >= kDefaultEntryCap) {
                diagnostics_.push_back(
                    "wallpaper library scan reached its entry cap"
                );
                break;
            }
            if (std::find(seen.begin(), seen.end(), path) != seen.end()) continue;
            seen.push_back(path);

            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error) {
                diagnostics_.push_back("ignored unreadable wallpaper: " + path.string());
                continue;
            }
            struct stat metadata {};
            const std::int64_t mtime = ::stat(path.c_str(), &metadata) == 0
                ? static_cast<std::int64_t>(metadata.st_mtime)
                : 0;

            WallpaperLibraryEntry entry;
            entry.path = path;
            entry.kind = classify(path);
            entry.origin = is_inside(path, roots_.cache_root)
                ? WallpaperOrigin::CachedOnline
                : root.origin;
            entry.file_size = size;
            entry.mtime = mtime;
            entry.fingerprint = fingerprint_of(path, size, mtime);
            entries_.push_back(std::move(entry));
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    // The scan duration travels with the snapshot, so the settings surface can
    // say how long indexing took without a second clock.
    last_scan_millis_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
    );
    return WallpaperLibrarySnapshot{entries_, diagnostics_, last_scan_millis_};
}

void WallpaperLibraryIndex::record_renderer_classification(
    const std::filesystem::path& path,
    WallpaperMediaKind kind
) {
    for (auto& entry : entries_) {
        if (entry.path != path) continue;
        // A classification that matches the fingerprint is the renderer talking
        // about this very file, not about whatever used to live at the path.
        if (entry.kind != kind) entry.kind = kind;
        return;
    }
}

std::vector<WallpaperLibraryEntry> WallpaperLibraryIndex::pool(
    WallpaperPool which
) const {
    std::vector<WallpaperLibraryEntry> result;
    for (const auto& entry : entries_) {
        const bool local = entry.origin == WallpaperOrigin::Local;
        const bool live = entry.kind == WallpaperMediaKind::Live;
        const bool wanted = which == WallpaperPool::LocalStatic
            ? (local && !live)
            : which == WallpaperPool::LocalLive
                ? (local && live)
                : which == WallpaperPool::OnlineStatic
                    ? (!local && !live)
                    : (!local && live);
        if (wanted) result.push_back(entry);
    }
    return result;
}

const WallpaperLibraryEntry* WallpaperLibraryIndex::find(
    const std::filesystem::path& path
) const {
    const auto found = std::find_if(
        entries_.begin(), entries_.end(),
        [&path](const WallpaperLibraryEntry& entry) { return entry.path == path; }
    );
    return found != entries_.end() ? &*found : nullptr;
}

std::filesystem::path default_wallpaper_cache_root() {
    if (const char* configured = std::getenv("REALMHEART_WALLPAPER_CACHE_ROOT");
        configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured);
    }
    if (const char* data_home = std::getenv("XDG_DATA_HOME");
        data_home != nullptr && *data_home != '\0') {
        return std::filesystem::path(data_home) / "realmheart/wallpaper-cache";
    }
    if (const char* home = std::getenv("HOME");
        home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / kCacheRoot;
    }
    return std::filesystem::temp_directory_path() / "realmheart/wallpaper-cache";
}

} // namespace realmheart::services
