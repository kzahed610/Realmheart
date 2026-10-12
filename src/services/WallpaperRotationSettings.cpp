#include "services/WallpaperRotationSettings.hpp"

#include "core/Command.hpp"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace realmheart::services {
namespace {

constexpr std::string_view kPoolNames[] = {
    "local-static",
    "local-live",
    "online-static",
    "online-live"
};

// The rotation state override follows the same convention as the rest of the
// shell's state paths, so a test can redirect it.
constexpr std::string_view kStateOverrideVariable =
    "REALMHEART_WALLPAPER_ROTATION_STATE";

std::string trimmed(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::vector<std::string> split_paths(const nlohmann::json& value) {
    std::vector<std::string> paths;
    if (!value.is_array()) return paths;
    for (const auto& entry : value) {
        if (!entry.is_string()) continue;
        paths.push_back(entry.get<std::string>());
    }
    return paths;
}

} // namespace

std::string_view wallpaper_pool_name(WallpaperPool pool) noexcept {
    const auto index = static_cast<std::size_t>(pool);
    if (index >= wallpaper_pool_count()) return {};
    return kPoolNames[index];
}

bool WallpaperRotationSettings::has_enabled_pool() const noexcept {
    for (const int weight : pool_weights) {
        if (weight > 0) return true;
    }
    return false;
}

nlohmann::json WallpaperRotationSettings::to_json() const {
    nlohmann::json source;
    source["schema_version"] = schema_version;
    source["enabled"] = enabled;
    source["interval_minutes"] = interval_minutes;
    source["static_root"] = static_root.string();
    source["live_root"] = live_root.string();
    source["pool_weights"] = nlohmann::json::object();
    for (std::size_t index = 0; index < pool_weights.size(); ++index) {
        source["pool_weights"][std::string(wallpaper_pool_name(
            static_cast<WallpaperPool>(index)
        ))] = pool_weights[index];
    }
    source["pause_rules"] = nlohmann::json::object();
    source["pause_rules"]["selector_open"] = pause_when_selector_open;
    source["pause_rules"]["session_locked"] = pause_when_locked;
    source["pause_rules"]["exclusive_fullscreen"] =
        pause_when_exclusive_fullscreen;
    source["pause_rules"]["critical_battery"] = pause_on_critical_battery;
    source["paused_outputs"] = paused_outputs;
    // The newest entry is the last one, so the file reads like a log.
    source["history"] = history;
    return source;
}

WallpaperRotationSettings WallpaperRotationSettings::from_json(
    const nlohmann::json& source,
    std::string* diagnostic
) {
    WallpaperRotationSettings settings;
    if (!source.is_object()) {
        if (diagnostic != nullptr) *diagnostic = "rotation settings are not an object";
        return settings;
    }
    if (diagnostic != nullptr) diagnostic->clear();

    // A version newer than this build understands is loaded as best-effort:
    // every field falls back to its default when it is missing or wrong, so an
    // unexpected upgrade cannot brick rotation.
    if (const auto version = source.find("schema_version");
        version != source.end() && version->is_number_integer()) {
        settings.schema_version = std::max(1, version->get<int>());
    } else if (diagnostic != nullptr) {
        *diagnostic = "rotation settings have no readable schema version";
    }

    if (const auto enabled = source.find("enabled");
        enabled != source.end() && enabled->is_boolean()) {
        settings.enabled = enabled->get<bool>();
    }
    if (const auto interval = source.find("interval_minutes");
        interval != source.end() && interval->is_number_integer()) {
        settings.interval_minutes =
            std::clamp(interval->get<int>(), kMinimumIntervalMinutes, kMaximumIntervalMinutes);
    }
    if (const auto root = source.find("static_root");
        root != source.end() && root->is_string()) {
        settings.static_root = root->get<std::string>();
    }
    if (const auto root = source.find("live_root");
        root != source.end() && root->is_string()) {
        settings.live_root = root->get<std::string>();
    }

    if (const auto weights = source.find("pool_weights");
        weights != source.end() && weights->is_object()) {
        for (std::size_t index = 0; index < settings.pool_weights.size(); ++index) {
            const auto found = weights->find(
                std::string(wallpaper_pool_name(static_cast<WallpaperPool>(index)))
            );
            if (found == weights->end() || !found->is_number_integer()) continue;
            // A weight is a slider from 0 to 100; anything outside is clamped
            // rather than rejected.
            settings.pool_weights[index] =
                std::clamp(found->get<int>(), 0, 100);
        }
    }

    // A pause rule that is missing, or not a boolean, keeps its default rather
    // than silently turning a protection off.
    if (const auto rules = source.find("pause_rules");
        rules != source.end() && rules->is_object()) {
        const auto read_pause_rule = [&rules](const char* key, bool fallback) {
            const auto found = rules->find(key);
            return found != rules->end() && found->is_boolean()
                ? found->get<bool>()
                : fallback;
        };
        settings.pause_when_selector_open =
            read_pause_rule("selector_open", settings.pause_when_selector_open);
        settings.pause_when_locked =
            read_pause_rule("session_locked", settings.pause_when_locked);
        settings.pause_when_exclusive_fullscreen = read_pause_rule(
            "exclusive_fullscreen", settings.pause_when_exclusive_fullscreen
        );
        settings.pause_on_critical_battery =
            read_pause_rule("critical_battery", settings.pause_on_critical_battery);
    }

    if (const auto outputs = source.find("paused_outputs");
        outputs != source.end() && outputs->is_array()) {
        for (const auto& entry : *outputs) {
            if (!entry.is_string()) continue;
            const auto name = trimmed(entry.get<std::string>());
            if (!name.empty()) settings.paused_outputs.insert(name);
        }
    }

    // The history is bounded: a settings file that has somehow grown a hundred
    // entries does not bring a hundred choices with it.
    constexpr std::size_t kMaxReadHistory = 4 * kDefaultHistoryLimit;
    settings.history = split_paths(source.value("history", nlohmann::json::array()));
    if (settings.history.size() > kMaxReadHistory) {
        settings.history.erase(
            settings.history.begin(),
            settings.history.end() -
                static_cast<std::ptrdiff_t>(kMaxReadHistory)
        );
    }
    return settings;
}

std::filesystem::path WallpaperRotationStore::default_path() {
    if (const char* configured = std::getenv(std::string(kStateOverrideVariable).c_str());
        configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured);
    }
    if (const char* state_home = std::getenv("XDG_STATE_HOME");
        state_home != nullptr && *state_home != '\0') {
        return std::filesystem::path(state_home) /
               "realmheart/wallpaper/rotation.json";
    }
    if (const char* home = std::getenv("HOME");
        home != nullptr && *home != '\0') {
        return std::filesystem::path(home) /
               ".local/state/realmheart/wallpaper/rotation.json";
    }
    return std::filesystem::temp_directory_path() /
           ("realmheart-" + std::to_string(static_cast<unsigned long>(::geteuid()))) /
           "wallpaper/rotation.json";
}

WallpaperRotationStore::WallpaperRotationStore(std::filesystem::path path)
    : path_(path.empty() ? default_path() : std::move(path)) {}

WallpaperRotationSettings WallpaperRotationStore::load(
    std::string* diagnostic
) const {
    WallpaperRotationSettings settings;
    if (diagnostic != nullptr) diagnostic->clear();

    std::error_code error;
    if (!std::filesystem::exists(path_, error) || error) {
        // Not an error: rotation has never been configured.
        return settings;
    }

    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        if (diagnostic != nullptr) {
            *diagnostic = "rotation settings could not be opened";
        }
        return settings;
    }
    std::ostringstream contents;
    contents << input.rdbuf();

    nlohmann::json source;
    try {
        source = nlohmann::json::parse(contents.str());
    } catch (const std::exception&) {
        if (diagnostic != nullptr) {
            *diagnostic = "rotation settings are not valid JSON";
        }
        return settings;
    }

    settings = WallpaperRotationSettings::from_json(source, diagnostic);
    return settings;
}

bool WallpaperRotationStore::save(
    const WallpaperRotationSettings& settings,
    std::string* error_message
) const {
    if (error_message != nullptr) error_message->clear();

    std::error_code error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            if (error_message != nullptr) {
                *error_message = "rotation settings directory could not be created";
            }
            return false;
        }
    }

    const auto serialized = settings.to_json().dump(2) + "\n";
    // Written beside the destination and renamed over it: a crash mid-write
    // leaves the previous settings intact rather than a truncated file.
    const auto temporary = path_.string() + ".partial";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error_message != nullptr) {
                *error_message = "rotation settings could not be written";
            }
            return false;
        }
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) {
            if (error_message != nullptr) {
                *error_message = "rotation settings could not be written";
            }
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    std::filesystem::rename(temporary, path_, error);
    if (error) {
        // A rename across a filesystem boundary fails; copy then swap instead.
        std::filesystem::copy_file(
            temporary, path_, std::filesystem::copy_options::overwrite_existing, error
        );
        std::filesystem::remove(temporary, error);
        if (error) {
            if (error_message != nullptr) {
                *error_message = "rotation settings could not be stored";
            }
            return false;
        }
    }
    return true;
}

std::filesystem::path default_static_wallpaper_root() {
    if (const char* configured = std::getenv("REALMHEART_WALLPAPER_STATIC_ROOT");
        configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured);
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return std::filesystem::temp_directory_path() / "realmheart/wallpapers";
    }
    return std::filesystem::path(home) / "Pictures/Wallpapers";
}

std::filesystem::path default_live_wallpaper_root() {
    const char* configured = std::getenv("REALMHEART_WALLPAPER_LIVE_ROOT");
    if (configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured);
    }
    return default_static_wallpaper_root() / "Live";
}

} // namespace realmheart::services
