#include "services/GameMode.hpp"

#include "core/Command.hpp"
#include "nlohmann_json/json.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace realmheart::services {
namespace {

using json = nlohmann::json;

struct OptionOverride {
    const char* name;
    const char* game_value;
};

constexpr OptionOverride kOverrides[] = {
    {"animations:enabled", "0"},
    {"decoration:shadow:enabled", "0"},
    {"decoration:blur:enabled", "0"},
    {"general:gaps_in", "0"},
    {"general:gaps_out", "0"},
    {"general:border_size", "1"},
    {"decoration:rounding", "0"},
    {"general:allow_tearing", "1"},
};

std::filesystem::path state_path() {
    if (const char* configured = std::getenv("REALMHEART_GAMEMODE_STATE");
        configured != nullptr && *configured != '\0') {
        return configured;
    }
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime != nullptr && *runtime != '\0') {
        return std::filesystem::path(runtime) / "realmheart/gamemode-state.json";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local/state/realmheart/gamemode-state.json";
    }
    return std::filesystem::temp_directory_path() / "realmheart-gamemode-state.json";
}

std::optional<std::string> option_value(
    std::string_view name,
    const realmheart::core::CommandOptions& options
) {
    const auto result = realmheart::core::run_capture(
        {"hyprctl", "getoption", std::string(name), "-j"},
        options
    );
    if (!result.succeeded() || result.truncated || result.output.empty()) return std::nullopt;

    try {
        const auto document = json::parse(result.output);
        if (document.contains("bool") && document["bool"].is_boolean()) {
            return document["bool"].get<bool>() ? "1" : "0";
        }
        if (document.contains("int") && document["int"].is_number_integer()) {
            return std::to_string(document["int"].get<long long>());
        }
        if (document.contains("float") && document["float"].is_number()) {
            std::ostringstream out;
            out << document["float"].get<double>();
            return out.str();
        }
        for (const char* key : {"str", "data"}) {
            if (document.contains(key) && document[key].is_string()) {
                return document[key].get<std::string>();
            }
        }
    } catch (const json::exception&) {
    }
    return std::nullopt;
}

std::optional<json> load_snapshot() {
    std::ifstream file(state_path(), std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    try {
        json snapshot;
        file >> snapshot;
        if (!snapshot.is_object() || !snapshot.contains("options") || !snapshot["options"].is_object()) {
            return std::nullopt;
        }
        return snapshot;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

bool save_snapshot(const json& snapshot) {
    const auto path = state_path();
    std::error_code error;
    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return false;
    }

    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        file << snapshot.dump(2) << '\n';
        if (!file.good()) return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

void remove_snapshot() {
    std::error_code error;
    std::filesystem::remove(state_path(), error);
}

std::vector<std::string> batch_arguments(const std::vector<std::pair<std::string, std::string>>& values) {
    std::string batch;
    for (const auto& [name, value] : values) {
        if (!batch.empty()) batch += "; ";
        batch += "keyword ";
        batch += name;
        batch += ' ';
        batch += value;
    }
    return {"hyprctl", "--batch", std::move(batch)};
}

std::optional<std::vector<std::pair<std::string, std::string>>> snapshot_values(
    const json& snapshot
) {
    if (!snapshot.is_object() || !snapshot.contains("options") ||
        !snapshot["options"].is_object()) {
        return std::nullopt;
    }

    std::vector<std::pair<std::string, std::string>> values;
    values.reserve(std::size(kOverrides));
    const auto& options_object = snapshot["options"];
    for (const auto& override : kOverrides) {
        if (!options_object.contains(override.name) ||
            !options_object[override.name].is_string()) {
            return std::nullopt;
        }
        values.emplace_back(override.name, options_object[override.name].get<std::string>());
    }
    return values;
}

std::vector<std::pair<std::string, std::string>> game_mode_values() {
    std::vector<std::pair<std::string, std::string>> values;
    values.reserve(std::size(kOverrides));
    for (const auto& override : kOverrides) {
        values.emplace_back(override.name, override.game_value);
    }
    return values;
}

bool options_match(
    const std::vector<std::pair<std::string, std::string>>& expected,
    const realmheart::core::CommandOptions& options
) {
    for (const auto& [name, expected_value] : expected) {
        const auto actual = option_value(name, options);
        if (!actual || *actual != expected_value) return false;
    }
    return true;
}

bool restore_snapshot(
    const json& snapshot,
    const realmheart::core::CommandOptions& options
) {
    const auto restore = snapshot_values(snapshot);
    if (!restore) return false;
    const auto write = realmheart::core::run_capture(batch_arguments(*restore), options);
    return write.succeeded() && options_match(*restore, options);
}

} // namespace

std::optional<GameModeState> GameMode::read(const realmheart::core::CommandOptions& options) {
    if (!realmheart::core::command_exists("hyprctl")) return std::nullopt;
    const auto snapshot = load_snapshot();
    if (!snapshot) return GameModeState{false};

    // Realmheart owns Game Mode only while its restoration marker exists and
    // every override still matches. This avoids both false positives and
    // silently claiming a partially-applied mode.
    return GameModeState{options_match(game_mode_values(), options)};
}

GameModeMutationResult GameMode::set_enabled(
    bool enabled,
    const realmheart::core::CommandOptions& options
) {
    GameModeMutationResult mutation;
    if (!realmheart::core::command_exists("hyprctl")) {
        mutation.error = "hyprctl not found";
        return mutation;
    }

    if (enabled) {
        auto snapshot = load_snapshot();
        if (snapshot && options_match(game_mode_values(), options)) {
            mutation.success = true;
            mutation.state.enabled = true;
            return mutation;
        }

        // Preserve an existing marker's original values. A stale or partially
        // applied Game Mode must never overwrite the user's pre-Game-Mode state.
        if (!snapshot) {
            json captured;
            captured["version"] = 1;
            captured["options"] = json::object();
            for (const auto& override : kOverrides) {
                const auto original = option_value(override.name, options);
                if (!original) {
                    mutation.error = std::string("Unable to snapshot Hyprland option: ") + override.name;
                    return mutation;
                }
                captured["options"][override.name] = *original;
            }
            if (!save_snapshot(captured)) {
                mutation.error = "Unable to persist Gamemode restoration snapshot";
                return mutation;
            }
            snapshot = std::move(captured);
        } else if (!snapshot_values(*snapshot)) {
            mutation.error = "Gamemode restoration snapshot is incomplete";
            return mutation;
        }

        const auto overrides = game_mode_values();
        const auto write = realmheart::core::run_capture(batch_arguments(overrides), options);
        if (!write.succeeded() || !options_match(overrides, options)) {
            // Hyprland batches are not assumed atomic. Restore the complete
            // original snapshot, and retain the marker if recovery itself fails.
            const bool restored = restore_snapshot(*snapshot, options);
            if (restored) remove_snapshot();
            mutation.error = write.succeeded()
                ? "Hyprland accepted the Gamemode batch but one or more options failed readback"
                : realmheart::core::command_failure_detail(
                      write,
                      "Hyprland rejected Gamemode settings"
                  );
            if (!restored) mutation.error += "; original settings could not be fully restored";
            return mutation;
        }

        mutation.success = true;
        mutation.state.enabled = true;
        return mutation;
    }

    const auto snapshot = load_snapshot();
    if (!snapshot) {
        mutation.success = true;
        mutation.state.enabled = false;
        return mutation;
    }
    if (!snapshot_values(*snapshot)) {
        mutation.error = "Gamemode restoration snapshot is incomplete";
        return mutation;
    }

    if (!restore_snapshot(*snapshot, options)) {
        mutation.error = "Unable to restore every pre-Gamemode Hyprland setting";
        return mutation;
    }

    remove_snapshot();
    mutation.success = true;
    mutation.state.enabled = false;
    return mutation;
}

} // namespace realmheart::services
