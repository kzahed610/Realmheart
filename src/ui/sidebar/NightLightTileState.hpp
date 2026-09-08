#pragma once

#include <optional>
#include <string_view>
#include <tuple>

namespace realmheart::ui::sidebar {

[[nodiscard]] constexpr std::tuple<std::string_view, bool, bool>
night_light_tile_presentation(
    std::optional<bool> live_enabled,
    bool recovery_available
) noexcept {
    if (live_enabled.has_value()) {
        return {
            *live_enabled ? std::string_view{"On"} : std::string_view{"Off"},
            *live_enabled,
            true,
        };
    }
    if (recovery_available) return {"Start", false, true};
    return {"Unavailable", false, false};
}

} // namespace realmheart::ui::sidebar
