#include "services/MatugenParser.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void parses_historical_json() {
    const std::string json = R"json({
        "colors": {
            "dark": {
                "primary": "#aabbcc",
                "secondary": "#112233",
                "tertiary": "#445566",
                "background": "#010203",
                "surface": "#111213",
                "surface_container": "#202122",
                "on_surface": "#fafafa",
                "on_surface_variant": "#cccccc",
                "outline": "#777777",
                "error": "#ff3344"
            }
        }
    })json";

    const auto palette = realmheart::services::MatugenParser::parse(json);
    require(palette.has_value(), "historical Matugen JSON should parse");
    require(palette->get("primary") == "#aabbcc", "wrong historical primary");
    require(palette->get("background") == "#010203", "wrong historical background");
    require(palette->get("text") == "#fafafa", "wrong historical text");
}

void parses_template_shaped_json() {
    const std::string json = R"json({
        "colors": {
            "primary": {"dark": {"color": "#de55ff"}},
            "secondary": {"dark": {"hex": "#5588ff"}},
            "tertiary": {"dark": "#ff5599"},
            "background": {"dark": {"color": "#090711"}},
            "surface": {"dark": {"color": "#161222"}},
            "surface_container": {"dark": {"color": "#251d33"}},
            "on_surface": {"dark": {"color": "#f7efff"}},
            "on_surface_variant": {"dark": {"color": "#cdbed8"}},
            "outline": {"dark": {"color": "#84768f"}},
            "error": {"dark": {"color": "#ff6677"}}
        }
    })json";

    const auto palette = realmheart::services::MatugenParser::parse(json);
    require(palette.has_value(), "template-shaped Matugen JSON should parse");
    require(palette->get("primary") == "#de55ff", "wrong new-format primary");
    require(palette->get("surface_variant") == "#251d33", "wrong surface variant");
}


void parses_json_surrounded_by_harmless_output() {
    const std::string output = R"text(matugen: generated palette
{
    "colors": {
        "dark": {
            "primary": "#cc66ff",
            "secondary": "#6699ff",
            "tertiary": "#ff66aa",
            "background": "#080510",
            "surface": "#151020",
            "surface_container": "#241a31",
            "on_surface": "#f8eeff",
            "on_surface_variant": "#d0bedc",
            "outline": "#887790",
            "error": "#ff6677"
        }
    }
}
complete
)text";

    const auto palette = realmheart::services::MatugenParser::parse(output);
    require(palette.has_value(), "Matugen JSON should survive harmless surrounding output");
    require(palette->get("primary") == "#cc66ff", "wrong noisy-output primary");
}

void rejects_incomplete_json() {
    const auto palette = realmheart::services::MatugenParser::parse(
        R"json({"colors":{"dark":{"primary":"#ffffff"}}})json"
    );
    require(!palette.has_value(), "incomplete palette must be rejected");
}

} // namespace

int main() {
    parses_historical_json();
    parses_template_shaped_json();
    parses_json_surrounded_by_harmless_output();
    rejects_incomplete_json();
    std::cout << "MatugenParserTests passed\n";
    return 0;
}
