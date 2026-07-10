#include "services/Brightness.hpp"

#include "core/Command.hpp"

#include <cstdlib>
#include <cmath>
#include <sstream>
#include <string>
#include <algorithm>

namespace realmheart::services {

std::optional<BrightnessState> Brightness::read(const realmheart::core::CommandOptions& options) {
    if (!realmheart::core::command_exists("brightnessctl")) return std::nullopt;
    auto current = realmheart::core::run_capture({"brightnessctl", "g"}, options);
    auto maximum = realmheart::core::run_capture({"brightnessctl", "m"}, options);
    if (!current.succeeded() || current.truncated || !maximum.succeeded() || maximum.truncated) {
        return std::nullopt;
    }

    BrightnessState state{};
    state.current = std::atoi(current.output.c_str());
    state.maximum = std::atoi(maximum.output.c_str());
    if (state.maximum > 0) state.percent = (static_cast<double>(state.current) / state.maximum) * 100.0;
    return state;
}

bool Brightness::set(int value, const realmheart::core::CommandOptions& options) {
    if (!realmheart::core::command_exists("brightnessctl")) return false;

    int clamped_value = std::clamp(value, 0, 100);
    std::string arg = std::to_string(clamped_value) + "%";
    const auto result = realmheart::core::run_capture({"brightnessctl", "set", arg}, options);
    if (!result.succeeded()) return false;

    const auto readback = read(options);
    return readback.has_value() && std::abs(readback->percent - clamped_value) <= 1.0;
}

} // namespace realmheart::services
