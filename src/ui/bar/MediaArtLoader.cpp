#include "ui/bar/MediaArtLoader.hpp"

#include "core/Command.hpp"
#include <algorithm>
#include <atomic>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

namespace realmheart::ui::bar {
namespace {

constexpr std::uintmax_t kMaximumArtBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumCachedArtwork = 64;

int hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return 10 + character - 'a';
    if (character >= 'A' && character <= 'F') return 10 + character - 'A';
    return -1;
}

std::optional<std::filesystem::path> local_path(std::string_view value) {
    std::string encoded;
    if (value.starts_with("file://")) {
        value.remove_prefix(7);
        if (value.starts_with("localhost/")) value.remove_prefix(9);
        if (!value.starts_with('/')) return std::nullopt;
        encoded.assign(value);
    } else {
        const std::filesystem::path path(value);
        if (!path.is_absolute()) return std::nullopt;
        encoded.assign(value);
    }

    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] == '%' && index + 2 < encoded.size()) {
            const int high = hex_value(encoded[index + 1]);
            const int low = hex_value(encoded[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        decoded.push_back(encoded[index]);
    }
    if (decoded.find('\0') != std::string::npos) return std::nullopt;

    const std::filesystem::path path(decoded);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return std::nullopt;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumArtBytes) return std::nullopt;
    return path;
}

bool is_remote_url(std::string_view value) {
    return value.starts_with("https://") || value.starts_with("http://");
}

std::filesystem::path cache_root() {
    if (const char* configured = std::getenv("XDG_CACHE_HOME");
        configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured) / "realmheart" / "media-art";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".cache" / "realmheart" / "media-art";
    }
    return std::filesystem::temp_directory_path() / "realmheart-media-art";
}

std::string cache_key(std::string_view value) {
    // Stable FNV-1a key: deterministic across processes, unlike std::hash.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

bool usable_cached_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return false;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size > 0 && size <= kMaximumArtBytes;
}

void prune_cache(const std::filesystem::path& root) {
    struct CachedEntry {
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
    };

    std::vector<CachedEntry> entries;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->path().extension() != ".art") continue;
        const auto modified = std::filesystem::last_write_time(iterator->path(), error);
        if (error) {
            error.clear();
            continue;
        }
        entries.push_back({iterator->path(), modified});
    }
    if (entries.size() <= kMaximumCachedArtwork) return;

    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.modified < right.modified;
    });
    const std::size_t remove_count = entries.size() - kMaximumCachedArtwork;
    for (std::size_t index = 0; index < remove_count; ++index) {
        std::filesystem::remove(entries[index].path, error);
        error.clear();
    }
}

std::optional<std::filesystem::path> fetch_remote(
    std::string_view url,
    const std::function<bool()>& cancelled
) {
    if (cancelled && cancelled()) return std::nullopt;

    std::error_code error;
    const auto root = cache_root();
    std::filesystem::create_directories(root, error);
    if (error) return std::nullopt;

    const auto target = root / (cache_key(url) + ".art");
    if (usable_cached_file(target)) return target;
    if (!realmheart::core::command_exists("curl")) return std::nullopt;

    static std::atomic<std::uint64_t> temporary_counter{0};
    const auto temporary = root /
        (cache_key(url) + ".part-" + std::to_string(static_cast<long long>(::getpid())) +
         "-" + std::to_string(++temporary_counter));
    std::filesystem::remove(temporary, error);
    error.clear();

    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::seconds(7);
    options.terminate_grace = std::chrono::milliseconds(150);
    options.max_output_bytes = 8 * 1024;
    options.cancelled = cancelled;

    const auto result = realmheart::core::run_capture({
        "curl",
        "--location",
        "--fail",
        "--silent",
        "--show-error",
        "--max-time", "6",
        "--max-filesize", std::to_string(kMaximumArtBytes),
        "--proto", "=http,https",
        "--proto-redir", "=http,https",
        "--output", temporary.string(),
        std::string(url),
    }, options);

    if (!result.succeeded() || !usable_cached_file(temporary) ||
        (cancelled && cancelled())) {
        std::filesystem::remove(temporary, error);
        return std::nullopt;
    }

    std::filesystem::rename(temporary, target, error);
    if (error) {
        // A concurrent task may have populated the same cache key first.
        std::filesystem::remove(temporary, error);
        if (!usable_cached_file(target)) return std::nullopt;
    }
    prune_cache(root);
    return target;
}

} // namespace

std::optional<std::filesystem::path> MediaArtLoader::resolve(
    std::string_view art_url,
    const std::function<bool()>& cancelled
) {
    if (art_url.empty()) return std::nullopt;
    if (const auto path = local_path(art_url)) return path;
    if (!is_remote_url(art_url)) return std::nullopt;
    return fetch_remote(art_url, cancelled);
}

} // namespace realmheart::ui::bar
