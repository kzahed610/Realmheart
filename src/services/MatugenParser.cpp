#include "services/MatugenParser.hpp"
#include "nlohmann_json/json.hpp"

#include <array>
#include <iostream>
#include <string_view>

namespace realmheart::services {
namespace {

using json = nlohmann::json;

std::optional<std::string> color_from_node(const json& node) {
    if (node.is_string()) {
        const auto value = node.get<std::string>();
        if (!value.empty()) return value;
        return std::nullopt;
    }

    if (!node.is_object()) return std::nullopt;

    for (const std::string_view key : {"color", "hex", "default"}) {
        const auto it = node.find(std::string(key));
        if (it != node.end()) {
            if (auto value = color_from_node(*it)) return value;
        }
    }
    return std::nullopt;
}

std::optional<std::string> read_color(
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
            if (auto value = color_from_node(*role_it)) return value;
        }
    }

    // New/template-shaped output:
    // { "colors": { "primary": { "dark": { "color": "#..." } } } }
    if (const auto role_it = colors.find(role_key); role_it != colors.end()) {
        if (role_it->is_object()) {
            if (const auto mode_it = role_it->find(mode_key); mode_it != role_it->end()) {
                if (auto value = color_from_node(*mode_it)) return value;
            }
            if (const auto default_it = role_it->find("default"); default_it != role_it->end()) {
                if (auto value = color_from_node(*default_it)) return value;
            }
        }
        if (auto value = color_from_node(*role_it)) return value;
    }

    return std::nullopt;
}

std::optional<std::string> first_role(
    const json& colors,
    std::string_view mode,
    std::initializer_list<std::string_view> roles
) {
    for (const auto role : roles) {
        if (auto value = read_color(colors, role, mode)) return value;
    }
    return std::nullopt;
}

void put_if(Palette& palette, std::string key, const std::optional<std::string>& value) {
    if (value && !value->empty()) palette.colors.emplace(std::move(key), *value);
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
        const auto error = first_role(colors, mode_key, {"error", "primary"});

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

        constexpr std::array<std::string_view, 4> required{
            "primary", "background", "surface", "text"
        };
        for (const auto key : required) {
            if (!palette.colors.contains(std::string(key))) {
                std::cerr << "[MatugenParser] Missing required color role: " << key << '\n';
                return std::nullopt;
            }
        }

        return palette;
    } catch (const json::exception& error) {
        std::cerr << "[MatugenParser] Unable to parse Matugen JSON: "
                  << error.what() << '\n';
        return std::nullopt;
    }
}

} // namespace realmheart::services
