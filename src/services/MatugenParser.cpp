#include "services/MatugenParser.hpp"
#include "nlohmann_json/json.hpp"

#include <array>
#include <iostream>
#include <string_view>

namespace realmheart::services {
namespace {

using json = nlohmann::json;

struct ColorReadResult {
    std::optional<std::string> value;
    bool present = false;
    bool invalid = false;
};

ColorReadResult color_from_node(const json& node) {
    if (node.is_string()) {
        const auto value = node.get<std::string>();
        if (is_valid_palette_color(value)) return {value, true, false};
        return {std::nullopt, true, true};
    }

    if (!node.is_object()) return {std::nullopt, true, true};

    for (const std::string_view key : {"color", "hex", "default"}) {
        const auto it = node.find(std::string(key));
        if (it != node.end()) {
            return color_from_node(*it);
        }
    }
    return {};
}

ColorReadResult read_color(
    const json& colors,
    std::string_view role,
    std::string_view mode
) {
    const std::string role_key(role);
    const std::string mode_key(mode);

    // Historical --old-json-output shape:
    // { "colors": { "dark": { "primary": "#..." } } }
    if (const auto mode_it = colors.find(mode_key);
        mode_it != colors.end() && mode_it->is_object()) {
        if (const auto role_it = mode_it->find(role_key); role_it != mode_it->end()) {
            return color_from_node(*role_it);
        }
    }

    // New/template-shaped output:
    // { "colors": { "primary": { "dark": { "color": "#..." } } } }
    if (const auto role_it = colors.find(role_key); role_it != colors.end()) {
        if (role_it->is_object()) {
            if (const auto mode_it = role_it->find(mode_key); mode_it != role_it->end()) {
                const auto result = color_from_node(*mode_it);
                if (result.present) return result;
            }
            if (const auto default_it = role_it->find("default"); default_it != role_it->end()) {
                const auto result = color_from_node(*default_it);
                if (result.present) return result;
            }
        }
        return color_from_node(*role_it);
    }

    return {};
}

ColorReadResult first_role(
    const json& colors,
    std::string_view mode,
    std::initializer_list<std::string_view> roles
) {
    for (const auto role : roles) {
        const auto result = read_color(colors, role, mode);
        if (result.invalid || result.value) return result;
    }
    return {};
}

void put_if(Palette& palette, std::string key, const ColorReadResult& value) {
    if (value.value) palette.colors.emplace(std::move(key), *value.value);
}

} // namespace

std::optional<Palette> MatugenParser::parse(const std::string& json_string, ThemeMode mode) {
    try {
        // `--quiet` should make Matugen emit only JSON. Keep this small guard so
        // one harmless warning or launcher prefix cannot permanently freeze the
        // active palette on the previous theme.
        const auto object_begin = json_string.find('{');
        const auto object_end = json_string.rfind('}');
        if (object_begin == std::string::npos || object_end == std::string::npos ||
            object_end < object_begin) {
            std::cerr << "[MatugenParser] Output does not contain a JSON object\n";
            return std::nullopt;
        }

        const auto data = json::parse(
            json_string.substr(object_begin, object_end - object_begin + 1)
        );
        const auto colors_it = data.find("colors");
        if (colors_it == data.end() || !colors_it->is_object()) {
            std::cerr << "[MatugenParser] JSON does not contain a colors object\n";
            return std::nullopt;
        }

        const std::string_view mode_key = mode == ThemeMode::Dark ? "dark" : "light";
        const json& colors = *colors_it;
        Palette palette;

        constexpr std::array<std::string_view, 14> declared_roles{
            "primary", "source_color", "secondary", "tertiary", "background",
            "surface", "surface_container", "surface_variant", "surface_container_low",
            "on_surface", "on_background", "on_surface_variant", "outline",
            "outline_variant"
        };
        for (const auto role : declared_roles) {
            if (read_color(colors, role, mode_key).invalid) {
                std::cerr << "[MatugenParser] Invalid color role: " << role << std::endl;
                return std::nullopt;
            }
        }

        const auto primary = first_role(colors, mode_key, {"primary", "source_color"});
        const auto secondary = first_role(colors, mode_key, {"secondary", "primary"});
        const auto tertiary = first_role(colors, mode_key, {"tertiary", "secondary", "primary"});
        const auto background = first_role(colors, mode_key, {"background", "surface"});
        const auto surface = first_role(colors, mode_key, {"surface", "surface_container", "background"});
        const auto surface_variant = first_role(
            colors,
            mode_key,
            {"surface_container", "surface_variant", "surface_container_low", "surface"}
        );
        const auto text = first_role(colors, mode_key, {"on_surface", "on_background"});
        const auto text_muted = first_role(colors, mode_key, {"on_surface_variant", "outline", "on_surface"});
        const auto outline = first_role(colors, mode_key, {"outline", "outline_variant"});
        const auto error = first_role(colors, mode_key, {"error"});

        put_if(palette, "primary", primary);
        put_if(palette, "accent", primary);
        put_if(palette, "secondary", secondary);
        put_if(palette, "tertiary", tertiary);
        put_if(palette, "background", background);
        put_if(palette, "surface", surface);
        put_if(palette, "surface_variant", surface_variant);
        put_if(palette, "text", text);
        put_if(palette, "text_muted", text_muted);
        put_if(palette, "outline", outline);
        put_if(palette, "error", error);
        put_if(palette, "red", error);
        put_if(palette, "blue", secondary);

        if (!error.value) {
            std::cerr << "[MatugenParser] Missing required semantic error color role" << std::endl;
            return std::nullopt;
        }

        constexpr std::array<std::string_view, 4> required{
            "primary", "background", "surface", "text"
        };
        for (const auto key : required) {
            if (!palette.colors.contains(std::string(key))) {
                std::cerr << "[MatugenParser] Missing required color role: " << key << std::endl;
                return std::nullopt;
            }
        }

        if (!palette_is_valid(palette)) {
            std::cerr << "[MatugenParser] Palette failed color validation" << std::endl;
            return std::nullopt;
        }

        return palette;
    } catch (const json::exception& error) {
        std::cerr << "[MatugenParser] Unable to parse Matugen JSON: "
                  << error.what() << std::endl;
        return std::nullopt;
    } catch (const std::exception& error) {
        std::cerr << "[MatugenParser] Unable to construct palette: "
                  << error.what() << std::endl;
        return std::nullopt;
    }
}

} // namespace realmheart::services
