#include "worldscar/WallpaperLibrary.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>
#include <system_error>

namespace realmheart::worldscar {
namespace {

bool supported_extension(std::string extension) {
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); }
    );

    constexpr std::string_view supported[] = {
        ".png", ".jpg", ".jpeg", ".webp"
    };
    return std::find(
        std::begin(supported),
        std::end(supported),
        extension
    ) != std::end(supported);
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

bool decodable_image(const std::filesystem::path& path) {
    GError* error = nullptr;
    // Decode a tiny scaled frame rather than trusting the extension/header.
    // This keeps discovery bounded while rejecting truncated/corrupt images
    // before a Worldscar session owns any GL state.
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_scale(
        path.c_str(),
        1,
        1,
        TRUE,
        &error
    );
    const bool valid = pixbuf != nullptr;
    if (pixbuf != nullptr) g_object_unref(pixbuf);
    g_clear_error(&error);
    return valid;
}

} // namespace

std::filesystem::path WallpaperLibrary::default_root() {
    if (const char* home = g_get_home_dir(); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / "Pictures" / "Wallpapers";
    }
    return {};
}

WallpaperDiscovery WallpaperLibrary::discover(std::filesystem::path root) const {
    WallpaperDiscovery result;
    if (root.empty()) root = default_root();
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
    while (iterator != end) {
        const std::filesystem::path path = iterator->path();

        std::error_code type_error;
        const bool regular = std::filesystem::is_regular_file(path, type_error);
        if (type_error) {
            result.diagnostics.emplace_back(
                "ignored unreadable entry: " + path.string()
            );
        } else if (regular && supported_extension(path.extension().string())) {
            if (decodable_image(path)) {
                result.paths.push_back(path);
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
        result.paths.begin(),
        result.paths.end(),
        [](const auto& left, const auto& right) {
            const std::string left_folded = lowercase_filename(left);
            const std::string right_folded = lowercase_filename(right);
            if (left_folded != right_folded) return left_folded < right_folded;
            return left.filename().string() < right.filename().string();
        }
    );

    return result;
}

} // namespace realmheart::worldscar
