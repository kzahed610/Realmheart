#pragma once

#include "core/Command.hpp"

#include <optional>
#include <string>

namespace realmheart::services {

struct GameModeState {
    bool enabled = false;
};

struct GameModeMutationResult {
    bool success = false;
    GameModeState state;
    std::string error;
};

class GameMode {
public:
    static std::optional<GameModeState> read(const realmheart::core::CommandOptions& options = {});
    static GameModeMutationResult set_enabled(
        bool enabled,
        const realmheart::core::CommandOptions& options = {}
    );
};

} // namespace realmheart::services
