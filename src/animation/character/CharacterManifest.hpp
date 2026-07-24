#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace realmheart::animation::character {

enum class CharacterPlane {
    Back,
    Front,
};

enum class CharacterLayerPlacement {
    SourceCanvas,
    Host,
};

enum class CharacterLayerRenderer {
    Static,
    HairMesh,
};

struct CharacterPoint {
    double x = 0.0;
    double y = 0.0;
};

struct CharacterSize {
    int width = 0;
    int height = 0;
};

struct CharacterAsset {
    std::string id;
    std::string file;
    std::string family;
    CharacterPoint offset;
    CharacterSize size;
    std::filesystem::path path;
};

struct CharacterLayer {
    std::string id;
    std::string asset_id;
    CharacterPlane plane = CharacterPlane::Back;
    CharacterLayerPlacement placement = CharacterLayerPlacement::SourceCanvas;
    CharacterLayerRenderer renderer = CharacterLayerRenderer::Static;
    CharacterPoint host_offset;
    std::string mask_asset_id;
    std::string flow_asset_id;
    int mesh_rows = 0;
    double mesh_strength = 1.0;
    double idle_strength = 0.0;
    double idle_phase = 0.0;
    double flow_strength = 0.0;
    double flow_frequency = 0.0;
    double flow_phase = 0.0;
    bool visible = true;
};

struct CharacterExpressionRig {
    bool enabled = false;
    std::string eyes_layer_id;
    std::string mouth_layer_id;
    std::string eyes_inward_asset_id;
    std::string eyes_half_asset_id;
    std::string eyes_closed_asset_id;
    std::string mouth_curious_asset_id;
};

struct CharacterPlacement {
    // Fraction of the host surface height occupied by the source canvas.
    double height_fraction = 1.0;

    // Normalized source-canvas point mapped to the host occlusion anchor.
    CharacterPoint source_anchor;

    // Logical-pixel offset from the host occlusion anchor.
    CharacterPoint host_offset;
};

class CharacterManifest {
public:
    static std::optional<CharacterManifest> load(
        const std::filesystem::path& character_root,
        int preferred_scale,
        std::string* error_message = nullptr
    );

    [[nodiscard]] const CharacterAsset* find_asset(std::string_view id) const;
    [[nodiscard]] const CharacterLayer* find_layer(std::string_view id) const;

    std::string character;
    std::string scale_variant;
    std::filesystem::path root;
    CharacterSize source_canvas;
    CharacterPlacement placement;
    CharacterExpressionRig expression;
    std::vector<CharacterLayer> layers;
    std::unordered_map<std::string, CharacterAsset> assets;
};

} // namespace realmheart::animation::character
