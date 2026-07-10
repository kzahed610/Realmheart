#pragma once

#include "core/Command.hpp"

#include <optional>

namespace realmheart::services {

struct BrightnessState {
    int current = 0;
    int maximum = 0;
    double percent = 0.0;
};

class Brightness {
public:
    static std::optional<BrightnessState> read(const realmheart::core::CommandOptions& options = {});
    static bool set(int value, const realmheart::core::CommandOptions& options = {});
};

} // namespace realmheart::services
