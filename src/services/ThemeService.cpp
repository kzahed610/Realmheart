#include "services/ThemeService.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace realmheart::services {
namespace {

constexpr std::string_view kCacheHeader = "realmheart-theme-cache-v1";
constexpr std::array<std::string_view, 4> kRequiredRoles{
    "primary", "background", "surface", "text"
};

std::filesystem::path default_cache_path() {
    if (const char* configured = std::getenv("REALMHEART_THEME_CACHE");
        configured != nullptr && *configured != '\0') {
        return configured;
    }
    if (const char* state_home = std::getenv("XDG_STATE_HOME");
        state_home != nullptr && *state_home != '\0') {
        return std::filesystem::path(state_home) / "realmheart/theme-palette.tsv";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) /
               ".local/state/realmheart/theme-palette.tsv";
    }
    return std::filesystem::temp_directory_path() /
           ("realmheart-" + std::to_string(static_cast<unsigned long>(::geteuid()))) /
           "theme-palette.tsv";
}

bool is_cache_token(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char character) {
        return character == '\t' || character == '\n' || character == '\r' ||
               std::iscntrl(character) != 0;
    });
}

bool is_css_hex_color(std::string_view value) {
    if (value.empty() || value.front() != '#') return false;
    const std::size_t digits = value.size() - 1;
    if (digits != 3 && digits != 4 && digits != 6 && digits != 8) return false;
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char character) {
        return std::isxdigit(character) != 0;
    });
}

bool palette_has_required_roles(const Palette& palette) {
    return std::all_of(kRequiredRoles.begin(), kRequiredRoles.end(), [&](std::string_view role) {
        const auto it = palette.colors.find(std::string(role));
        return it != palette.colors.end() && is_css_hex_color(it->second);
    });
}

std::optional<Palette> load_cached_palette(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;

    std::string line;
    if (!std::getline(input, line)) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != kCacheHeader) return std::nullopt;

    Palette palette;
    std::size_t entry_count = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() > 320 || ++entry_count > 64) return std::nullopt;

        const auto separator = line.find('\t');
        if (separator == std::string::npos || line.find('\t', separator + 1) != std::string::npos) {
            return std::nullopt;
        }

        std::string key = line.substr(0, separator);
        std::string value = line.substr(separator + 1);
        if (!is_cache_token(key) || !is_css_hex_color(value) ||
            palette.colors.contains(key)) {
            return std::nullopt;
        }
        palette.colors.emplace(std::move(key), std::move(value));
    }

    if (!input.eof() || !palette_has_required_roles(palette)) return std::nullopt;
    return palette;
}

bool persist_cached_palette(const std::filesystem::path& path, const Palette& palette) {
    if (!palette_has_required_roles(palette)) return false;

    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(palette.colors.size());
    for (const auto& [key, value] : palette.colors) {
        if (!is_cache_token(key) || !is_css_hex_color(value)) continue;
        entries.emplace_back(key, value);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });

    std::error_code error;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return false;
    }

    const auto temporary = std::filesystem::path(
        path.string() + ".tmp-" + std::to_string(static_cast<unsigned long>(::getpid()))
    );
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << kCacheHeader << '\n';
        for (const auto& [key, value] : entries) {
            output << key << '\t' << value << '\n';
        }
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        return false;
    }
    return true;
}

} // namespace

ThemeService::Subscription::~Subscription() {
    reset();
}

ThemeService::Subscription::Subscription(Subscription&& other) noexcept
    : registry_(std::move(other.registry_)), id_(std::exchange(other.id_, 0)) {}

ThemeService::Subscription& ThemeService::Subscription::operator=(Subscription&& other) noexcept {
    if (this == &other) return *this;
    reset();
    registry_ = std::move(other.registry_);
    id_ = std::exchange(other.id_, 0);
    return *this;
}

void ThemeService::Subscription::reset() {
    if (id_ == 0) return;
    if (const auto registry = registry_.lock()) {
        std::lock_guard lock(registry->mutex);
        registry->callbacks.erase(id_);
    }
    registry_.reset();
    id_ = 0;
}

ThemeService::ThemeService()
    : cache_path_(default_cache_path()) {
    palette_.colors = {
        {"primary", "#cba6f7"},
        {"accent", "#cba6f7"},
        {"secondary", "#89b4fa"},
        {"tertiary", "#f5c2e7"},
        {"background", "#11111b"},
        {"surface", "#1e1e2e"},
        {"surface_variant", "#313244"},
        {"text", "#cdd6f4"},
        {"text_muted", "#a6adc8"},
        {"outline", "#45475a"},
        {"error", "#f38ba8"},
        {"red", "#f38ba8"},
        {"blue", "#89b4fa"}
    };

    if (auto cached = load_cached_palette(cache_path_)) {
        palette_ = std::move(*cached);
        std::cout << "[ThemeService] Restored cached palette. Primary="
                  << palette_.get("primary")
                  << " background=" << palette_.get("background") << '\n';
    } else {
        std::error_code error;
        if (std::filesystem::exists(cache_path_, error) && !error) {
            std::cerr << "[ThemeService] Ignoring invalid palette cache: "
                      << cache_path_ << '\n';
        }
    }
}

Palette ThemeService::get_palette() const {
    std::lock_guard lock(palette_mutex_);
    return palette_;
}

void ThemeService::update_palette(Palette new_palette) {
    std::vector<ThemeChangedCallback> callbacks;
    Palette snapshot;

    {
        std::lock_guard lock(palette_mutex_);
        palette_ = std::move(new_palette);
        snapshot = palette_;
    }

    if (!persist_cached_palette(cache_path_, snapshot)) {
        std::cerr << "[ThemeService] Unable to persist palette cache: "
                  << cache_path_ << '\n';
    }

    {
        std::lock_guard lock(subscribers_->mutex);
        callbacks.reserve(subscribers_->callbacks.size());
        for (const auto& [_, callback] : subscribers_->callbacks) {
            callbacks.push_back(callback);
        }
    }

    std::cout << "[ThemeService] Palette updated. Primary="
              << snapshot.get("primary")
              << " background=" << snapshot.get("background") << '\n';

    for (auto& callback : callbacks) {
        if (callback) callback(snapshot);
    }
}

ThemeService::Subscription ThemeService::subscribe(ThemeChangedCallback callback) {
    if (!callback) return {};

    std::lock_guard lock(subscribers_->mutex);
    const std::size_t id = subscribers_->next_id++;
    subscribers_->callbacks.emplace(id, std::move(callback));
    return Subscription{subscribers_, id};
}

} // namespace realmheart::services
