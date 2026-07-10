#pragma once

#include <optional>
#include <string>

namespace realmheart::services {

struct AudioState {
    std::string raw_status;
};

class Audio {
public:
    static std::optional<AudioState> read_default_sink();
};

} // namespace realmheart::services
