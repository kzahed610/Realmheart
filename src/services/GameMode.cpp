#include "services/GameMode.hpp"

#include "core/Command.hpp"

#include <charconv>
#include <cctype>

namespace realmheart::services {
namespace {

constexpr const char* kEnableBatch =
    "keyword animations:enabled 0; "
    "keyword decoration:shadow:enabled 0; "
    "keyword decoration:blur:enabled 0; "
    "keyword general:gaps_in 0; "
    "keyword general:gaps_out 0; "
    "keyword general:border_size 1; "
    "keyword decoration:rounding 0; "
    "keyword general:allow_tearing 1";

std::optional<bool> parse_animations_enabled(const std::string& json) {
    const auto bool_key = json.find("\"bool\"");
    if (bool_key != std::string::npos) {
        const auto colon = json.find(':', bool_key + 6);
        if (colon == std::string::npos) return std::nullopt;

        const char* begin = json.data() + colon + 1;
        const char* end = json.data() + json.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
        if (end - begin >= 4 && std::string_view(begin, 4) == "true") return true;
        if (end - begin >= 5 && std::string_view(begin, 5) == "false") return false;
        return std::nullopt;
    }

    const auto key = json.find("\"int\"");
    if (key == std::string::npos) return std::nullopt;
    const auto colon = json.find(':', key + 5);
    if (colon == std::string::npos) return std::nullopt;

    const char* begin = json.data() + colon + 1;
    const char* end = json.data() + json.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(*begin))) ++begin;

    int value = 0;
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{}) return std::nullopt;
    return value != 0;
}

} // namespace

std::optional<GameModeState> GameMode::read(const realmheart::core::CommandOptions& options) {
    if (!realmheart::core::command_exists("hyprctl")) return std::nullopt;
    const auto result = realmheart::core::run_capture(
        {"hyprctl", "getoption", "animations:enabled", "-j"},
        options
    );
    if (!result.succeeded() || result.truncated) return std::nullopt;

    const auto animations_enabled = parse_animations_enabled(result.output);
    if (!animations_enabled) return std::nullopt;
    return GameModeState{!*animations_enabled};
}

GameModeMutationResult GameMode::set_enabled(
    bool enabled,
    const realmheart::core::CommandOptions& options
) {
    GameModeMutationResult mutation;
    const auto arguments = enabled
        ? std::vector<std::string>{"hyprctl", "--batch", kEnableBatch}
        : std::vector<std::string>{"hyprctl", "reload"};

    const auto write = realmheart::core::run_capture(arguments, options);
    if (!write.succeeded()) {
        mutation.error = realmheart::core::command_failure_detail(write, "hyprctl gamemode mutation failed");
        return mutation;
    }

    const auto readback = read(options);
    if (!readback) {
        mutation.error = "Gamemode state unavailable after mutation";
        return mutation;
    }

    mutation.state = *readback;
    mutation.success = mutation.state.enabled == enabled;
    if (!mutation.success) mutation.error = "Gamemode readback did not match requested state";
    return mutation;
}

} // namespace realmheart::services
