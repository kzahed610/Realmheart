#include "services/MatugenParser.hpp"
#include <iostream>

namespace realmheart::services {

std::optional<Palette> MatugenParser::parse(const std::string& json_string, ThemeMode mode) {
    using json = nlohmann::json;
    
    try {
        auto data = json::parse(json_string);
        
        // Matugen's structure: "colors" -> [key] -> [mode] -> "color"
        if (!data.contains("colors")) {
            return std::nullopt;
        }

        const std::string mode_key = (mode == ThemeMode::Dark) ? "dark" : "light";
        Palette palette;
        
        for (const auto& key : COLOR_MAPPING) {
            if (data["colors"].contains(key)) {
                auto color_node = data["colors"][key];
                if (color_node.is_object() && color_node.contains(mode_key)) {
                    // Extract the hex color string
                    std::string hex_color = color_node[mode_key].value("color", "");
                    if (!hex_color.empty()) {
                        palette.colors[key] = hex_color;
                    }
                }
            }
        }

        // Special case: "text" often maps to "on_background" or "on_surface" in Material design
        if (palette.colors.find("text") == palette.colors.end()) {
            if (data["colors"].contains("on_background")) {
                auto node = data["colors"]["on_background"];
                if (node.is_object() && node.contains(mode_key)) {
                    palette.colors["text"] = node[mode_key].value("color", "");
                }
            }
        }

        return palette;
    } catch (const json::parse_error& e) {
        // Silent failure to allow fallback to Catppuccin
        return std::nullopt;
    }
}

} // namespace realmheart::services
