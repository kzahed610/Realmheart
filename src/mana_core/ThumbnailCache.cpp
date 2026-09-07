#include "mana_core/ThumbnailCache.hpp"

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <limits>
#include <unistd.h>
#include <optional>
#include <cmath>
#include <atomic>
#include <vector>

namespace realmheart::mana_core {
namespace {

constexpr std::array<char, 8> kPreviewCacheMagic{
    'R', 'H', 'W', 'S', 'R', 'A', 'W', '2'
};

constexpr std::uint64_t kMaxSourceBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSourcePixels = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMaxSourceDimension = 16384;
constexpr std::uint32_t kMaxPreviewDimension = 4096;
constexpr std::uintmax_t kMaxCacheBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxCacheEntries = 256;

struct PreviewSourceStamp {
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

std::optional<PreviewSourceStamp> preview_source_stamp(const std::filesystem::path& path) noexcept {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error) return std::nullopt;

    std::error_code time_error;
    const auto modified = std::filesystem::last_write_time(path, time_error);
    if (time_error) return std::nullopt;

    PreviewSourceStamp stamp;
    stamp.size = static_cast<std::uint64_t>(size);
    stamp.mtime = static_cast<std::int64_t>(modified.time_since_epoch().count());
    return stamp;
}

std::uint64_t fnv1a_64(std::string_view text) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : text) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::filesystem::path preview_cache_path(
    const std::filesystem::path& source,
    int target_dimension
) {
    std::error_code canonical_error;
    auto canonical = std::filesystem::weakly_canonical(source, canonical_error);
    if (canonical_error) canonical = source.lexically_normal();

    const std::string key = canonical.generic_string() + "\n" +
        std::to_string(std::max(target_dimension, 0));
    char name[32]{};
    std::snprintf(name, sizeof(name), "%016llx.raw", static_cast<unsigned long long>(fnv1a_64(key)));

    const char* user_cache = g_get_user_cache_dir();
    std::filesystem::path root;
    if (user_cache != nullptr && *user_cache != '\0') {
        root = std::filesystem::path(user_cache);
    } else {
        std::error_code temporary_error;
        root = std::filesystem::temp_directory_path(temporary_error);
        if (temporary_error || root.empty()) root = "/tmp";
    }
    return root / "realmheart" / "mana-core-thumbnails" / name;
}

template <typename Value>
bool read_binary(std::ifstream& input, Value& value) {
    input.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(Value)));
    return static_cast<bool>(input);
}

template <typename Value>
bool write_binary(std::ofstream& output, const Value& value) {
    output.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(Value)));
    return static_cast<bool>(output);
}

void free_pixbuf_data(guchar* pixels, gpointer) {
    std::free(pixels);
}

void remove_cache_file(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::remove(path, error);
}

void prune_cache() noexcept {
    const char* user_cache = g_get_user_cache_dir();
    if (user_cache == nullptr || *user_cache == '\0') return;
    const auto root = std::filesystem::path(user_cache) /
        "realmheart" / "mana-core-thumbnails";
    std::error_code iterator_error;
    std::vector<std::filesystem::directory_entry> entries;
    for (std::filesystem::directory_iterator iterator(
             root,
             std::filesystem::directory_options::skip_permission_denied,
             iterator_error), end;
         iterator != end && !iterator_error;
         iterator.increment(iterator_error)) {
        std::error_code type_error;
        if (iterator->is_regular_file(type_error) && !type_error &&
            iterator->path().extension() == ".raw") {
            entries.push_back(*iterator);
        }
    }
    if (iterator_error ||
        (entries.size() <= kMaxCacheEntries &&
         [&entries] {
             std::uintmax_t total = 0;
             for (const auto& entry : entries) {
                 std::error_code error;
                 total += entry.file_size(error);
             }
             return total <= kMaxCacheBytes;
         }())) {
        return;
    }

    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        std::error_code left_error;
        std::error_code right_error;
        const auto left_time = left.last_write_time(left_error);
        const auto right_time = right.last_write_time(right_error);
        if (left_error || right_error) return left.path().string() < right.path().string();
        return left_time < right_time;
    });

    auto total_size = [&entries] {
        std::uintmax_t total = 0;
        for (const auto& entry : entries) {
            std::error_code error;
            total += entry.file_size(error);
        }
        return total;
    };
    while (!entries.empty() &&
           (entries.size() > kMaxCacheEntries || total_size() > kMaxCacheBytes)) {
        remove_cache_file(entries.front().path());
        entries.erase(entries.begin());
    }
}

GdkPixbuf* load_cached_preview(
    const std::filesystem::path& source,
    int target_dimension
) {
    const auto cache = preview_cache_path(source, target_dimension);
    const auto stamp = preview_source_stamp(source);
    if (!stamp) {
        remove_cache_file(cache);
        return nullptr;
    }
    std::ifstream input(cache, std::ios::binary);
    if (!input) return nullptr;

    std::array<char, 8> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!input || magic != kPreviewCacheMagic) {
        remove_cache_file(cache);
        return nullptr;
    }

    std::uint64_t source_size = 0;
    std::int64_t source_mtime = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 0;
    std::uint32_t cached_target_dimension = 0;
    if (!read_binary(input, source_size) ||
        !read_binary(input, source_mtime) ||
        !read_binary(input, width) ||
        !read_binary(input, height) ||
        !read_binary(input, channels) ||
        !read_binary(input, cached_target_dimension)) {
        remove_cache_file(cache);
        return nullptr;
    }

    if (source_size != stamp->size || source_mtime != stamp->mtime ||
        cached_target_dimension != static_cast<std::uint32_t>(std::max(target_dimension, 0)) ||
        width == 0 || height == 0 ||
        width > kMaxPreviewDimension || height > kMaxPreviewDimension ||
        static_cast<std::uint64_t>(width) * height > kMaxSourcePixels ||
        (channels != 3U && channels != 4U)) {
        remove_cache_file(cache);
        return nullptr;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
    if (row_bytes == 0 || static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / row_bytes) {
        return nullptr;
    }
    const std::size_t byte_count = row_bytes * static_cast<std::size_t>(height);

    guchar* pixels = static_cast<guchar*>(std::malloc(byte_count));
    if (!pixels) return nullptr;

    input.read(reinterpret_cast<char*>(pixels), static_cast<std::streamsize>(byte_count));
    if (!input) {
        std::free(pixels);
        return nullptr;
    }

    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_data(
        pixels,
        GDK_COLORSPACE_RGB,
        channels == 4,
        8,
        static_cast<int>(width),
        static_cast<int>(height),
        static_cast<int>(row_bytes),
        free_pixbuf_data,
        nullptr
    );

    if (!pixbuf) {
        std::free(pixels);
    }
    return pixbuf;
}

void store_cached_preview(
    const std::filesystem::path& source,
    int target_dimension,
    GdkPixbuf* pixbuf
) noexcept {
    const auto stamp = preview_source_stamp(source);
    if (!stamp || !pixbuf) return;
    
    const int width = gdk_pixbuf_get_width(pixbuf);
    const int height = gdk_pixbuf_get_height(pixbuf);
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const int row_stride = gdk_pixbuf_get_rowstride(pixbuf);
    const guchar* pixels = gdk_pixbuf_read_pixels(pixbuf);

    if (width <= 0 || height <= 0 ||
        width > static_cast<int>(kMaxPreviewDimension) ||
        height > static_cast<int>(kMaxPreviewDimension) ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > kMaxSourcePixels ||
        (channels != 3 && channels != 4) || !pixels) {
        return;
    }

    const int row_bytes = width * channels;
    if (row_bytes <= 0 || row_stride < row_bytes) return;

    const auto cache = preview_cache_path(source, target_dimension);
    std::error_code directory_error;
    std::filesystem::create_directories(cache.parent_path(), directory_error);
    if (directory_error) return;

    static std::atomic<std::uint64_t> temp_counter{0};
    const auto temporary = cache.string() + ".tmp." +
        std::to_string(static_cast<long long>(::getpid())) + "." +
        std::to_string(temp_counter.fetch_add(1) + 1);

    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;

    output.write(kPreviewCacheMagic.data(), static_cast<std::streamsize>(kPreviewCacheMagic.size()));
    const std::uint32_t w = static_cast<std::uint32_t>(width);
    const std::uint32_t h = static_cast<std::uint32_t>(height);
    const std::uint32_t c = static_cast<std::uint32_t>(channels);
    if (!write_binary(output, stamp->size) ||
        !write_binary(output, stamp->mtime) ||
        !write_binary(output, w) ||
        !write_binary(output, h) ||
        !write_binary(output, c) ||
        !write_binary(output, static_cast<std::uint32_t>(std::max(target_dimension, 0)))) {
        output.close();
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return;
    }

    for (int row = 0; row < height; ++row) {
        output.write(
            reinterpret_cast<const char*>(pixels + static_cast<std::size_t>(row) * static_cast<std::size_t>(row_stride)),
            static_cast<std::streamsize>(row_bytes)
        );
        if (!output) break;
    }
    output.close();
    if (!output) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return;
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary, cache, rename_error);
    if (rename_error) {
        std::error_code remove_destination_error;
        std::filesystem::remove(cache, remove_destination_error);
        rename_error.clear();
        std::filesystem::rename(temporary, cache, rename_error);
    }
    if (rename_error) {
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
    }
}

} // namespace

GdkPixbuf* ThumbnailCache::load_or_create(
    const std::filesystem::path& source_path,
    int target_dimension,
    std::string* error
) {
    if (target_dimension < 0 ||
        static_cast<std::uint32_t>(target_dimension) > kMaxPreviewDimension) {
        if (error) *error = "thumbnail target dimension is out of bounds";
        return nullptr;
    }

    std::error_code file_error;
    const auto source_size = std::filesystem::file_size(source_path, file_error);
    if (file_error || source_size == 0 || source_size > kMaxSourceBytes) {
        if (error) *error = "wallpaper file is missing or exceeds the size limit";
        return nullptr;
    }

    int source_width = 0;
    int source_height = 0;
    static_cast<void>(gdk_pixbuf_get_file_info(source_path.c_str(), &source_width, &source_height));
    if (source_width <= 0 || source_height <= 0 ||
        static_cast<std::uint32_t>(source_width) > kMaxSourceDimension ||
        static_cast<std::uint32_t>(source_height) > kMaxSourceDimension ||
        static_cast<std::uint64_t>(source_width) * static_cast<std::uint64_t>(source_height) > kMaxSourcePixels) {
        if (error) *error = "wallpaper dimensions exceed the decode limit";
        return nullptr;
    }

    if (GdkPixbuf* cached = load_cached_preview(source_path, target_dimension)) {
        if (error) error->clear();
        return cached;
    }

    int target_width = source_width;
    int target_height = source_height;
    if (target_dimension > 0 && source_width > 0 && source_height > 0 &&
        std::max(source_width, source_height) > target_dimension) {
        const double scale = static_cast<double>(target_dimension) / static_cast<double>(std::max(source_width, source_height));
        target_width = std::max(1, static_cast<int>(std::lround(source_width * scale)));
        target_height = std::max(1, static_cast<int>(std::lround(source_height * scale)));
    }

    GError* decode_error = nullptr;
    GdkPixbuf* pixbuf = nullptr;
    if (target_width > 0 && target_height > 0 && (target_width != source_width || target_height != source_height)) {
        // preserve_aspect_ratio = TRUE fixes distortion
        pixbuf = gdk_pixbuf_new_from_file_at_scale(source_path.c_str(), target_width, target_height, TRUE, &decode_error);
    } else {
        pixbuf = gdk_pixbuf_new_from_file(source_path.c_str(), &decode_error);
    }

    if (!pixbuf) {
        if (error) {
            *error = decode_error && decode_error->message ? decode_error->message : "unable to decode wallpaper";
        }
        g_clear_error(&decode_error);
        return nullptr;
    }
    g_clear_error(&decode_error);

    store_cached_preview(source_path, target_dimension, pixbuf);
    prune_cache();
    
    if (error) error->clear();
    return pixbuf;
}

} // namespace realmheart::mana_core
