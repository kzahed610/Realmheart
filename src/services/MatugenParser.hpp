#pragma once

#include "services/ThemeService.hpp"
#include <optional>
#include <string>

namespace realmheart::services {

enum class ThemeMode {
    Dark,
    Light
};

class MatugenParser {
public:
    // Supports both the historical Matugen JSON dump and the newer/template-shaped
    // color object formats. This keeps Realmheart compatible across Matugen releases.
    static std::optional<Palette> parse(
        const std::string& json_string,
        ThemeMode mode = ThemeMode::Dark
    );
};

} // namespace realmheart::services
