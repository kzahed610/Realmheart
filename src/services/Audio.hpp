#pragma once

#include "core/Command.hpp"

#include <optional>
#include <string>

namespace realmheart::services {

struct AudioState {
    std::string raw_status;
    double volume = 0.0;
    bool muted = false;
};

struct AudioMutationResult {
    bool success = false;
    AudioState state;
    std::string error;
};

class Audio {
public:
    static std::optional<AudioState> read_default_sink(
        const realmheart::core::CommandOptions& options = {}
    );
    static AudioMutationResult set_default_sink_volume(
        double volume,
        const realmheart::core::CommandOptions& options = {}
    );
};

} // namespace realmheart::services
