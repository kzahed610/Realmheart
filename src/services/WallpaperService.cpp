#include "services/WallpaperService.hpp"
#include "core/Command.hpp"
#include "services/ThemeService.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <filesystem>
#include <vector>
#include <iostream>

#include <unistd.h>

namespace realmheart::services {
namespace {

std::filesystem::path default_state_file() {
    if (const char* state_home = std::getenv("XDG_STATE_HOME"); state_home && *state_home) {
        return std::filesystem::path(state_home) / "realmheart/wallpaper/path.txt";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local/state/realmheart/wallpaper/path.txt";
    }
    return std::filesystem::temp_directory_path() /
           ("realmheart-" + std::to_string(static_cast<unsigned long>(::geteuid()))) /
           "wallpaper/path.txt";
}

bool is_supported_extension(std::string extension) {
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });

    constexpr std::string_view supported[] = {
        ".jpg", ".jpeg", ".png", ".webp", ".avif", ".bmp", ".tif", ".tiff"
    };
    return std::find(std::begin(supported), std::end(supported), extension) != std::end(supported);
}

} // namespace

WallpaperService::WallpaperService(std::filesystem::path state_file)
    : state_file_(state_file.empty() ? default_state_file() : std::move(state_file)) {}

bool WallpaperService::validate_image(const std::filesystem::path& path) const {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) return false;
    return is_supported_extension(path.extension().string());
}

std::optional<std::filesystem::path> WallpaperService::load_path() const {
    std::ifstream input(state_file_);
    std::string saved_path;
    if (!input || !std::getline(input, saved_path) || saved_path.empty()) return std::nullopt;
    if (!saved_path.empty() && saved_path.back() == '\r') saved_path.pop_back();
    if (saved_path.empty()) return std::nullopt;

    std::filesystem::path path(saved_path);
    if (!validate_image(path)) return std::nullopt;
    return path;
}

bool WallpaperService::persist_path(const std::filesystem::path& path) const {
    if (!validate_image(path)) return false;

    std::error_code error;
    auto absolute_path = std::filesystem::absolute(path, error);
    if (error) return false;
    absolute_path = absolute_path.lexically_normal();

    const auto parent = state_file_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return false;
    }

    const auto temporary = state_file_.string() + ".tmp-" +
                           std::to_string(static_cast<unsigned long>(::getpid()));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << absolute_path.string() << '\n';
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
        output.close();
        if (output.fail()) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    std::filesystem::rename(temporary, state_file_, error);
    if (error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        return false;
    }
    return true;
}

bool WallpaperService::update_state(const std::filesystem::path& path) {
    return persist_path(path);
}

bool WallpaperService::set_wallpaper(const std::string& path) {
    return persist_path(path);
}

bool WallpaperService::choose_wallpaper() {
    return false;
}

bool WallpaperService::generate_colors() {
    const auto path = load_path();
    if (!path) return false;

    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::seconds(10);
    options.max_output_bytes = 512 * 1024;
    const auto result = realmheart::core::run_capture({
        "matugen", "image", path->string(),
        "--dry-run", "--json", "hex", "--old-json-output",
        "--source-color-index", "0", "--quiet"
    }, options);
    return result.succeeded() && !result.output.empty() && !result.truncated;
}

} // namespace realmheart::services
