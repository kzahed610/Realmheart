#include "services/GameMode.hpp"

#include "core/Command.hpp"
#include "nlohmann_json/json.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace realmheart::services {
namespace {

using json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::size_t kMaximumSnapshotBytes = 16 * 1024;

enum class OptionKind { Boolean, Integer };

struct OptionOverride {
    const char* name;
    const char* game_value;
    OptionKind kind;
    int minimum;
    int maximum;
};

constexpr OptionOverride kOverrides[] = {
    {"animations:enabled", "0", OptionKind::Boolean, 0, 1},
    {"decoration:shadow:enabled", "0", OptionKind::Boolean, 0, 1},
    {"decoration:blur:enabled", "0", OptionKind::Boolean, 0, 1},
    {"general:gaps_in", "0", OptionKind::Integer, 0, 100},
    {"general:gaps_out", "0", OptionKind::Integer, 0, 100},
    {"general:border_size", "1", OptionKind::Integer, 0, 20},
    {"decoration:rounding", "0", OptionKind::Integer, 0, 100},
    {"general:allow_tearing", "1", OptionKind::Boolean, 0, 1},
};

std::mutex& transaction_mutex() {
    static auto* mutex = new std::mutex();
    return *mutex;
}

std::uint64_t next_token() {
    static std::atomic<std::uint64_t> token{1};
    return token.fetch_add(1, std::memory_order_relaxed);
}

realmheart::core::CommandOptions operation_options(
    const realmheart::core::CommandOptions& input
) {
    auto options = input;
    if (options.deadline == 1500ms) options.deadline = 5s;
    const auto requested_deadline = std::chrono::steady_clock::now() + options.deadline;
    if (!options.deadline_at || requested_deadline < *options.deadline_at) {
        options.deadline_at = requested_deadline;
    }
    return options;
}

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
    return std::filesystem::path("/tmp") /
        ("realmheart-" + std::to_string(::getuid())) / "gamemode-state.json";
}

const OptionOverride* override_for(std::string_view name) {
    for (const auto& override : kOverrides) {
        if (name == override.name) return &override;
    }
    return nullptr;
}

bool valid_option_value(const OptionOverride& option, std::string_view value) {
    if (option.kind == OptionKind::Boolean) return value == "0" || value == "1";
    int parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size() &&
        parsed >= option.minimum && parsed <= option.maximum;
}

bool private_regular_file(const std::filesystem::path& path, std::size_t* size = nullptr) {
    struct stat metadata {};
    if (::lstat(path.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode)) return false;
    if (metadata.st_uid != ::getuid() || (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0) return false;
    if (size != nullptr) *size = static_cast<std::size_t>(metadata.st_size);
    return true;
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
    const auto path = state_path();
    std::size_t size = 0;
    if (!private_regular_file(path, &size) || size > kMaximumSnapshotBytes) return std::nullopt;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;
    std::string contents(size, '\0');
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file.good() && !file.eof()) return std::nullopt;
    try {
        const auto snapshot = json::parse(contents);
        if (!snapshot.is_object() || snapshot.value("version", 0) != 1 ||
            !snapshot.contains("token") || !snapshot["token"].is_number_unsigned() ||
            snapshot["token"].get<std::uint64_t>() == 0 ||
            !snapshot.contains("options") || !snapshot["options"].is_object()) {
            return std::nullopt;
        }
        return snapshot;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

bool save_snapshot(const json& snapshot) {
    const auto path = state_path();
    const auto serialized = snapshot.dump(2) + "\n";
    if (serialized.size() > kMaximumSnapshotBytes) return false;

    std::error_code error;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return false;
        if (::chmod(parent.c_str(), 0700) != 0) return false;
        struct stat parent_metadata {};
        if (::stat(parent.c_str(), &parent_metadata) != 0 ||
            parent_metadata.st_uid != ::getuid() || (parent_metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            return false;
        }
    }

    const auto temporary = std::filesystem::path(path.string() + ".tmp." + std::to_string(::getpid()));
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (descriptor < 0) return false;
    std::size_t written = 0;
    while (written < serialized.size()) {
        const ssize_t count = ::write(descriptor, serialized.data() + written, serialized.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        ::close(descriptor);
        ::unlink(temporary.c_str());
        return false;
    }
    static_cast<void>(::fchmod(descriptor, 0600));
    static_cast<void>(::fsync(descriptor));
    ::close(descriptor);
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return false;
    }
    return true;
}

bool remove_snapshot_if_token(std::uint64_t token) {
    const auto snapshot = load_snapshot();
    if (!snapshot || (*snapshot)["token"].get<std::uint64_t>() != token) return false;
    std::error_code error;
    return std::filesystem::remove(state_path(), error) && !error;
}

std::optional<std::vector<std::pair<std::string, std::string>>> snapshot_values(
    const json& snapshot
) {
    if (!snapshot.is_object() || snapshot.value("version", 0) != 1 ||
        !snapshot.contains("token") || !snapshot["token"].is_number_unsigned() ||
        snapshot["token"].get<std::uint64_t>() == 0 ||
        !snapshot.contains("options") || !snapshot["options"].is_object()) {
        return std::nullopt;
    }

    std::vector<std::pair<std::string, std::string>> values;
    values.reserve(std::size(kOverrides));
    const auto& options_object = snapshot["options"];
    for (const auto& override : kOverrides) {
        if (!options_object.contains(override.name) ||
            !options_object[override.name].is_string()) return std::nullopt;
        const auto value = options_object[override.name].get<std::string>();
        if (!valid_option_value(override, value)) return std::nullopt;
        values.emplace_back(override.name, value);
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
        if (options.deadline_at && std::chrono::steady_clock::now() >= *options.deadline_at) return false;
        const auto actual = option_value(name, options);
        if (!actual || *actual != expected_value) return false;
    }
    return true;
}

std::vector<std::string> batch_arguments(
    const std::vector<std::pair<std::string, std::string>>& values
) {
    std::string batch;
    for (const auto& [name, value] : values) {
        const auto* option = override_for(name);
        if (option == nullptr || !valid_option_value(*option, value)) return {};
        if (!batch.empty()) batch += "; ";
        batch += "keyword ";
        batch += name;
        batch += ' ';
        batch += value;
    }
    return {"hyprctl", "--batch", std::move(batch)};
}

bool restore_snapshot(
    const json& snapshot,
    const realmheart::core::CommandOptions& options
) {
    const auto restore = snapshot_values(snapshot);
    if (!restore) return false;
    const auto command = batch_arguments(*restore);
    if (command.empty()) return false;
    const auto write = realmheart::core::run_capture(command, options);
    return write.succeeded() && options_match(*restore, options);
}

} // namespace

std::optional<GameModeState> GameMode::read(const realmheart::core::CommandOptions& input) {
    std::lock_guard lock(transaction_mutex());
    if (!realmheart::core::command_exists("hyprctl")) return std::nullopt;
    const auto options = operation_options(input);
    const auto snapshot = load_snapshot();
    if (!snapshot) return GameModeState{false};
    return GameModeState{options_match(game_mode_values(), options)};
}

GameModeMutationResult GameMode::set_enabled(
    bool enabled,
    const realmheart::core::CommandOptions& input
) {
    std::lock_guard lock(transaction_mutex());
    GameModeMutationResult mutation;
    if (!realmheart::core::command_exists("hyprctl")) {
        mutation.error = "hyprctl not found";
        return mutation;
    }
    const auto options = operation_options(input);

    if (enabled) {
        auto snapshot = load_snapshot();
        if (snapshot && options_match(game_mode_values(), options)) {
            mutation.success = true;
            mutation.state.enabled = true;
            return mutation;
        }

        if (!snapshot) {
            json captured;
            captured["version"] = 1;
            captured["token"] = next_token();
            captured["options"] = json::object();
            for (const auto& override : kOverrides) {
                const auto original = option_value(override.name, options);
                if (!original || !valid_option_value(override, *original)) {
                    mutation.error = std::string("Unable to snapshot valid Hyprland option: ") + override.name;
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
            mutation.error = "Gamemode restoration snapshot is invalid";
            return mutation;
        }

        const auto overrides = game_mode_values();
        const auto command = batch_arguments(overrides);
        const auto write = command.empty()
            ? realmheart::core::CommandResult{}
            : realmheart::core::run_capture(command, options);
        if (!write.succeeded() || !options_match(overrides, options)) {
            const bool restored = restore_snapshot(*snapshot, options);
            if (restored) {
                static_cast<void>(remove_snapshot_if_token((*snapshot)["token"].get<std::uint64_t>()));
            }
            mutation.error = write.succeeded()
                ? "Hyprland accepted the Gamemode batch but one or more options failed readback"
                : realmheart::core::command_failure_detail(write, "Hyprland rejected Gamemode settings");
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
    const auto token = (*snapshot).value("token", 0ULL);
    if (!snapshot_values(*snapshot) || token == 0) {
        mutation.error = "Gamemode restoration snapshot is invalid";
        return mutation;
    }
    if (!restore_snapshot(*snapshot, options)) {
        mutation.error = "Unable to restore every pre-Gamemode Hyprland setting";
        return mutation;
    }
    if (!remove_snapshot_if_token(token)) {
        mutation.error = "Gamemode restored, but ownership marker changed before removal";
        mutation.state.enabled = true;
        return mutation;
    }
    mutation.success = true;
    mutation.state.enabled = false;
    return mutation;
}

} // namespace realmheart::services
