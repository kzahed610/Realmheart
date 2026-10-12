#include "services/WallpaperPosterCache.hpp"

#include "core/Command.hpp"

#include <glib.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace realmheart::services {
namespace {

constexpr std::string_view kPosterCacheRoot =
    "realmheart/wallpaper-posters";
constexpr std::string_view kPngMagic = "\x89PNG\r\n\x1a\n";
// A PNG ends with a zero-length "IEND" chunk plus its CRC.
constexpr std::size_t kPngTrailerBytes = 12;

// Wallpapers the renderer can decode as a moving image. This mirrors the
// extensions the selector previews; a file that lies about its extension is
// classified by the renderer at apply time, not here.
constexpr std::array<std::string_view, 6> kLiveWallpaperExtensions{
    ".mp4", ".m4v", ".webm", ".mkv", ".mov", ".avi"
};

std::string lowercased(std::string text) {
    std::transform(
        text.begin(), text.end(), text.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        }
    );
    return text;
}

std::uint64_t fnv1a_64(std::string_view text) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::filesystem::path poster_cache_root() {
    // The same env-override convention as the rest of the shell's state paths,
    // so a test (or a user with a small cache volume) can redirect it.
    if (const char* override_root =
            std::getenv("REALMHEART_WALLPAPER_POSTER_CACHE");
        override_root != nullptr && *override_root != '\0') {
        return std::filesystem::path(override_root);
    }
    const char* user_cache = g_get_user_cache_dir();
    std::filesystem::path root;
    if (user_cache != nullptr && *user_cache != '\0') {
        root = std::filesystem::path(user_cache);
    } else {
        std::error_code error;
        root = std::filesystem::temp_directory_path(error);
        if (error || root.empty()) root = "/tmp";
    }
    return root / kPosterCacheRoot;
}

std::string hash_name(std::uint64_t value) {
    char name[32]{};
    std::snprintf(
        name, sizeof(name), "%016llx.png",
        static_cast<unsigned long long>(value)
    );
    return name;
}

// True when the file is a complete PNG. Both ends matter: a header alone is
// exactly what an interrupted extraction leaves behind, and theming from a
// truncated poster would build a palette out of half an image.
bool poster_file_is_complete(const std::filesystem::path& path) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error || size < kPngMagic.size() + kPngTrailerBytes) return false;

    std::ifstream input(path, std::ios::binary);
    if (!input) return false;

    std::array<char, 8> header{};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(header.size())) {
        return false;
    }
    if (std::string_view(header.data(), header.size()) != kPngMagic) return false;

    std::array<char, kPngTrailerBytes> trailer{};
    input.seekg(
        static_cast<std::streamoff>(size) -
            static_cast<std::streamoff>(trailer.size()),
        std::ios::beg
    );
    input.read(trailer.data(), static_cast<std::streamsize>(trailer.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(trailer.size())) {
        return false;
    }
    // Bytes 0..3 are the zero chunk length, 4..7 are "IEND", 8..11 the CRC.
    return trailer[4] == 'I' && trailer[5] == 'E' &&
           trailer[6] == 'N' && trailer[7] == 'D';
}

} // namespace

bool is_live_wallpaper_source(const std::filesystem::path& path) noexcept {
    if (path.empty()) return false;
    const std::string extension = lowercased(path.extension().string());
    if (extension.empty()) return false;
    return std::find(
               kLiveWallpaperExtensions.begin(),
               kLiveWallpaperExtensions.end(),
               extension
           ) != kLiveWallpaperExtensions.end();
}

std::uint64_t WallpaperPosterCache::source_fingerprint(
    const std::filesystem::path& video
) noexcept {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(video, error);
    const std::string identity =
        (error ? video.lexically_normal() : canonical).generic_string();

    // The stat identity is what keeps a video replaced at the same path from
    // reusing the poster of the file that used to live there.
    std::string stamp;
    struct stat metadata {};
    if (::stat(video.c_str(), &metadata) == 0) {
        stamp = std::to_string(static_cast<unsigned long long>(metadata.st_size));
        stamp += ':';
        stamp += std::to_string(static_cast<long long>(metadata.st_mtime));
        stamp += ':';
        stamp += std::to_string(static_cast<unsigned long long>(metadata.st_ino));
    }
    return fnv1a_64(identity + "\n" + stamp);
}

std::filesystem::path WallpaperPosterCache::poster_path_for(
    const std::filesystem::path& video,
    int target_dimension
) {
    const auto dimension = static_cast<std::uint64_t>(std::max(target_dimension, 0));
    const auto identity = fnv1a_64(
        std::to_string(dimension) + "\n" + std::to_string(source_fingerprint(video))
    );
    return poster_cache_root() / hash_name(identity);
}

std::optional<std::filesystem::path> WallpaperPosterCache::poster_frame_for(
    const std::filesystem::path& video,
    int target_dimension,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (video.empty()) {
        if (error_message != nullptr) *error_message = "live wallpaper path is empty";
        return std::nullopt;
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(video, error) || error) {
        if (error_message != nullptr) {
            *error_message = "live wallpaper file is missing";
        }
        return std::nullopt;
    }

    const int dimension = target_dimension > 0
        ? target_dimension
        : wallpaper_poster_palette_dimension();
    const auto poster = poster_path_for(video, dimension);
    // A cached poster is the whole point: theming a live wallpaper must not
    // decode the video again every time.
    if (poster_file_is_complete(poster)) return poster;

    std::error_code directory_error;
    std::filesystem::create_directories(poster.parent_path(), directory_error);
    if (directory_error) {
        if (error_message != nullptr) {
            *error_message = "unable to create the wallpaper poster cache";
        }
        return std::nullopt;
    }

    // Written under a temporary name so a failed, interrupted, or concurrent
    // extraction never leaves a half-written PNG where a complete one belongs.
    // The encoder is named explicitly: the temporary name deliberately ends in
    // ".partial", and ffmpeg would otherwise pick an encoder from the extension
    // and write a JPEG into a file everything else treats as a PNG.
    const auto temporary = poster.string() + ".partial";
    const std::vector<std::string> argv = {
        "ffmpeg",
        "-hide_banner", "-loglevel", "error", "-y",
        "-i", video.string(),
        "-frames:v", "1",
        "-vf", "scale=" + std::to_string(dimension) + ":-2:flags=area",
        "-c:v", "png",
        "-f", "image2",
        temporary,
    };

    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::seconds(15);
    options.max_output_bytes = 4 * 1024;
    options.separate_stderr = true;
    const auto result = realmheart::core::run_capture(argv, options);

    std::error_code remove_error;
    if (!result.succeeded() ||
        !std::filesystem::is_regular_file(temporary, error) || error ||
        !poster_file_is_complete(temporary)) {
        std::filesystem::remove(temporary, remove_error);
        if (error_message != nullptr) {
            *error_message =
                "ffmpeg could not extract a poster frame from the live "
                "wallpaper: " +
                realmheart::core::command_failure_detail(
                    result, "poster extraction failed"
                );
        }
        return std::nullopt;
    }

    std::filesystem::rename(temporary, poster, error);
    if (error) {
        // Another extraction may have won the race and published first, which
        // is fine; the poster is usable either way.
        std::error_code copy_error;
        std::filesystem::copy_file(
            temporary,
            poster,
            std::filesystem::copy_options::overwrite_existing,
            copy_error
        );
        std::filesystem::remove(temporary, remove_error);
        if (copy_error) {
            if (error_message != nullptr) {
                *error_message = "unable to publish the live wallpaper poster frame";
            }
            return std::nullopt;
        }
    }
    return poster;
}

std::optional<std::filesystem::path> WallpaperPosterCache::palette_source_for(
    const std::filesystem::path& wallpaper,
    std::string* error_message
) {
    if (error_message != nullptr) error_message->clear();
    if (wallpaper.empty()) {
        if (error_message != nullptr) {
            *error_message = "wallpaper source path is empty";
        }
        return std::nullopt;
    }
    if (!is_live_wallpaper_source(wallpaper)) return wallpaper;
    return poster_frame_for(
        wallpaper, wallpaper_poster_palette_dimension(), error_message
    );
}

void WallpaperPosterCache::prune() {
    std::error_code error;
    const auto root = poster_cache_root();
    if (!std::filesystem::is_directory(root, error) || error) return;

    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type modified{};
    };
    std::vector<Entry> entries;
    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (!iterator->is_regular_file()) continue;
        if (iterator->path().extension() != ".png") continue;
        const auto modified = iterator->last_write_time(error);
        if (error) break;
        entries.push_back(Entry{iterator->path(), modified});
    }
    if (error || entries.size() <= wallpaper_poster_cache_max_entries()) return;

    // Oldest first: a poster that has served its purpose is the one to lose.
    std::sort(
        entries.begin(), entries.end(),
        [](const Entry& left, const Entry& right) {
            return left.modified < right.modified;
        }
    );
    for (std::size_t index = 0;
         index < entries.size() - wallpaper_poster_cache_max_entries();
         ++index) {
        std::error_code remove_error;
        std::filesystem::remove(entries[index].path, remove_error);
    }
}

} // namespace realmheart::services
