#pragma once

#include <string>
#include <optional>
#include "services/ThemeService.hpp"
#include "nlohmann_json/json.hpp"

namespace realmheart::services {

enum class ThemeMode {
    Dark,
    Light
};

class MatugenParser {
public:
    /**
     * Parses a Matugen JSON output string and extracts a Palette.
     * @param json_string The raw JSON output from `matugen image ... --json hex`
     * @param mode The desired theme mode (Dark or Light)
     * @return A Palette containing the extracted colors, or std::nullopt if parsing fails.
     */
    static std::optional<Palette> parse(const std::string& json_string, ThemeMode mode = ThemeMode::Dark);

private:
    // Mapping of internal Palette keys to Matugen JSON keys
    static inline const std::vector<std::string> COLOR_MAPPING = {
        "primary",
        "secondary",
        "tertiary",
        "background",
        "surface",
        "text",
        "outline"
    };
};

} // namespace realmheart::services
