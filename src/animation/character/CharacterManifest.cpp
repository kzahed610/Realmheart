#include "animation/character/CharacterManifest.hpp"

#include "nlohmann_json/json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <system_error>
#include <utility>

namespace realmheart::animation::character {
namespace {

using Json = nlohmann::json;

void set_error(std::string* target, std::string message) {
    if (target != nullptr) *target = std::move(message);
}

std::optional<Json> read_json(
    const std::filesystem::path& path,
    std::string* error_message
) {
    std::ifstream input(path);
    if (!input) {
        set_error(error_message, "Unable to open character manifest: " + path.string());
        return std::nullopt;
    }

    try {
        return Json::parse(input);
    } catch (const std::exception& error) {
        set_error(
            error_message,
            "Unable to parse character manifest " + path.string() + ": " + error.what()
        );
        return std::nullopt;
    }
}

std::optional<CharacterPoint> read_point(
    const Json& parent,
    std::string_view key,
    std::string* error_message
) {
    try {
        const auto& value = parent.at(std::string(key));
        CharacterPoint point{
            .x = value.at("x").get<double>(),
            .y = value.at("y").get<double>(),
        };
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            set_error(error_message, "Character point contains a non-finite value");
            return std::nullopt;
        }
        return point;
    } catch (const std::exception& error) {
        set_error(
            error_message,
            "Invalid character point '" + std::string(key) + "': " + error.what()
        );
        return std::nullopt;
    }
}

std::optional<CharacterPlane> read_plane(
    std::string_view value,
    std::string* error_message
) {
    if (value == "back") return CharacterPlane::Back;
    if (value == "front") return CharacterPlane::Front;
    set_error(error_message, "Unknown character layer plane: " + std::string(value));
    return std::nullopt;
}

std::optional<CharacterLayerPlacement> read_layer_placement(
    std::string_view value,
    std::string* error_message
) {
    if (value == "source") return CharacterLayerPlacement::SourceCanvas;
    if (value == "host") return CharacterLayerPlacement::Host;
    set_error(error_message, "Unknown character layer placement: " + std::string(value));
    return std::nullopt;
}

std::optional<CharacterLayerRenderer> read_layer_renderer(
    std::string_view value,
    std::string* error_message
) {
    if (value == "static") return CharacterLayerRenderer::Static;
    if (value == "mesh") return CharacterLayerRenderer::HairMesh;
    set_error(error_message, "Unknown character layer renderer: " + std::string(value));
    return std::nullopt;
}

struct SelectedManifestPath {
    std::filesystem::path path;
    core::DisplayTier tier = core::DisplayTier::P1080;
};

std::optional<SelectedManifestPath> display_manifest_path(
    const std::filesystem::path& root,
    core::DisplayTier requested_tier
) {
    std::error_code error;
    const auto candidate = root /
        std::string(core::display_tier_directory(requested_tier)) /
        "manifest.json";
    if (std::filesystem::is_regular_file(candidate, error) && !error) {
        return SelectedManifestPath{
            .path = candidate,
            .tier = requested_tier,
        };
    }

    // A missing higher tier falls back to the canonical 1080p package. Do not
    // fall back to the legacy density directories: they are not part of the
    // display-tier contract and may contain incompatible crop metadata.
    if (requested_tier == core::DisplayTier::P1080) return std::nullopt;
    error.clear();
    const auto fallback = root / "1080p" / "manifest.json";
    if (std::filesystem::is_regular_file(fallback, error) && !error) {
        return SelectedManifestPath{
            .path = fallback,
            .tier = core::DisplayTier::P1080,
        };
    }
    return std::nullopt;
}

bool validate_family_geometry(
    const Json& scale_json,
    const CharacterManifest& manifest,
    std::string* error_message
) {
    if (!scale_json.contains("families")) return true;

    try {
        for (const auto& [family_name, family_json] : scale_json.at("families").items()) {
            const auto& members = family_json.at("members");
            if (!members.is_array() || members.empty()) continue;

            const CharacterAsset* first = nullptr;
            for (const auto& member_json : members) {
                const std::string member = member_json.get<std::string>();
                const auto* asset = manifest.find_asset(member);
                if (asset == nullptr) {
                    set_error(
                        error_message,
                        "Family '" + family_name + "' references missing asset '" + member + "'"
                    );
                    return false;
                }
                if (first == nullptr) {
                    first = asset;
                    continue;
                }
                if (asset->size.width != first->size.width ||
                    asset->size.height != first->size.height ||
                    asset->offset.x != first->offset.x ||
                    asset->offset.y != first->offset.y) {
                    set_error(
                        error_message,
                        "Family '" + family_name + "' does not share identical geometry"
                    );
                    return false;
                }
            }
        }
    } catch (const std::exception& error) {
        set_error(error_message, "Invalid character family contract: " + std::string(error.what()));
        return false;
    }
    return true;
}

} // namespace

CharacterPoint host_layer_offset_for_display_tier(
    CharacterPoint canonical_offset,
    core::DisplayTier display_tier
) noexcept {
    const double layout_scale = core::display_tier_spec(display_tier).scale;
    return {
        .x = canonical_offset.x * layout_scale,
        .y = canonical_offset.y * layout_scale,
    };
}

std::optional<CharacterManifest> CharacterManifest::load(
    const std::filesystem::path& character_root,
    core::DisplayTier display_tier,
    std::string* error_message
) {
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(character_root, error);
    if (error || !std::filesystem::is_directory(canonical_root, error) || error) {
        set_error(error_message, "Character asset root does not exist: " + character_root.string());
        return std::nullopt;
    }

    const auto rig_json = read_json(canonical_root / "rig.json", error_message);
    if (!rig_json) return std::nullopt;

    const auto selected_manifest_path = display_manifest_path(canonical_root, display_tier);
    if (!selected_manifest_path) {
        set_error(error_message, "No explicit display-tier character manifest is available");
        return std::nullopt;
    }
    const auto scale_json = read_json(selected_manifest_path->path, error_message);
    if (!scale_json) return std::nullopt;

    CharacterManifest manifest;
    manifest.root = canonical_root;

    try {
        manifest.character = rig_json->at("character").get<std::string>();
        const auto manifest_tier = scale_json->at("displayTier").get<std::string>();
        const auto expected_tier = std::string(
            core::display_tier_directory(selected_manifest_path->tier)
        );
        if (manifest_tier != expected_tier) {
            set_error(
                error_message,
                "Character manifest display tier does not match its selected directory"
            );
            return std::nullopt;
        }
        manifest.display_tier = selected_manifest_path->tier;
        manifest.source_canvas = {
            .width = scale_json->at("sourceCanvas").at("width").get<int>(),
            .height = scale_json->at("sourceCanvas").at("height").get<int>(),
        };
        if (manifest.character.empty() || manifest.source_canvas.width <= 0 ||
            manifest.source_canvas.height <= 0) {
            set_error(error_message, "Character manifest has an invalid identity or source canvas");
            return std::nullopt;
        }

        const auto& placement_json = rig_json->at("placement");
        manifest.placement.height_fraction = placement_json.at("heightFraction").get<double>();
        const auto source_anchor = read_point(
            placement_json, "sourceAnchor", error_message
        );
        const auto host_offset = read_point(
            placement_json, "hostOffset", error_message
        );
        if (!source_anchor || !host_offset) return std::nullopt;
        manifest.placement.source_anchor = *source_anchor;
        manifest.placement.host_offset = *host_offset;

        if (!std::isfinite(manifest.placement.height_fraction) ||
            manifest.placement.height_fraction <= 0.0 ||
            manifest.placement.height_fraction > 2.0 ||
            manifest.placement.source_anchor.x < 0.0 ||
            manifest.placement.source_anchor.x > 1.0 ||
            manifest.placement.source_anchor.y < 0.0 ||
            manifest.placement.source_anchor.y > 1.0) {
            set_error(error_message, "Character placement is outside its accepted range");
            return std::nullopt;
        }

        const auto scale_directory = selected_manifest_path->path.parent_path();
        for (const auto& [asset_id, asset_json] : scale_json->at("assets").items()) {
            CharacterAsset asset;
            asset.id = asset_id;
            asset.file = asset_json.at("file").get<std::string>();
            asset.family = asset_json.at("family").get<std::string>();
            asset.offset = {
                .x = asset_json.at("offset").at("x").get<double>(),
                .y = asset_json.at("offset").at("y").get<double>(),
            };
            asset.size = {
                .width = asset_json.at("size").at("width").get<int>(),
                .height = asset_json.at("size").at("height").get<int>(),
            };
            asset.path = scale_directory / asset.file;

            if (asset.id.empty() || asset.file.empty() || asset.family.empty() ||
                asset.size.width <= 0 || asset.size.height <= 0 ||
                !std::isfinite(asset.offset.x) || !std::isfinite(asset.offset.y)) {
                set_error(error_message, "Character asset '" + asset_id + "' is invalid");
                return std::nullopt;
            }

            const auto canonical_asset = std::filesystem::weakly_canonical(asset.path, error);
            if (error || !std::filesystem::is_regular_file(canonical_asset, error) || error) {
                set_error(error_message, "Character asset file is missing: " + asset.path.string());
                return std::nullopt;
            }
            const auto mismatch = std::mismatch(
                canonical_root.begin(), canonical_root.end(),
                canonical_asset.begin(), canonical_asset.end()
            );
            if (mismatch.first != canonical_root.end()) {
                set_error(error_message, "Character asset escapes its asset root: " + asset.path.string());
                return std::nullopt;
            }
            asset.path = canonical_asset;
            manifest.assets.emplace(asset.id, std::move(asset));
        }

        for (const auto& layer_json : rig_json->at("layers")) {
            CharacterLayer layer;
            layer.id = layer_json.at("id").get<std::string>();
            layer.asset_id = layer_json.at("asset").get<std::string>();
            layer.visible = layer_json.value("visible", true);

            const auto plane = read_plane(
                layer_json.at("plane").get<std::string>(), error_message
            );
            const auto placement = read_layer_placement(
                layer_json.value("placement", std::string("source")), error_message
            );
            const auto renderer = read_layer_renderer(
                layer_json.value("renderer", std::string("static")), error_message
            );
            if (!plane || !placement || !renderer) return std::nullopt;
            layer.plane = *plane;
            layer.placement = *placement;
            layer.renderer = *renderer;

            if (layer_json.contains("hostOffset")) {
                const auto layer_offset = read_point(
                    layer_json, "hostOffset", error_message
                );
                if (!layer_offset) return std::nullopt;
                layer.host_offset = *layer_offset;
            }

            if (layer.id.empty() || manifest.find_layer(layer.id) != nullptr) {
                set_error(error_message, "Character layer id is empty or duplicated: " + layer.id);
                return std::nullopt;
            }
            const CharacterAsset* texture_asset = manifest.find_asset(layer.asset_id);
            if (texture_asset == nullptr) {
                set_error(
                    error_message,
                    "Character layer '" + layer.id + "' references missing asset '" +
                        layer.asset_id + "'"
                );
                return std::nullopt;
            }

            if (layer.renderer == CharacterLayerRenderer::HairMesh) {
                layer.mask_asset_id = layer_json.at("mask").get<std::string>();
                layer.mesh_rows = layer_json.value("meshRows", 24);
                layer.mesh_strength = layer_json.value("meshStrength", 1.0);
                layer.idle_strength = layer_json.value("idleStrength", 0.0);
                layer.idle_phase = layer_json.value("idlePhase", 0.0);
                layer.flow_asset_id = layer_json.value("flow", std::string{});
                layer.flow_strength = layer_json.value("flowStrength", 0.0);
                layer.flow_frequency = layer_json.value("flowFrequency", 0.0);
                layer.flow_phase = layer_json.value("flowPhase", 0.0);

                const CharacterAsset* mask_asset = manifest.find_asset(
                    layer.mask_asset_id
                );
                const CharacterAsset* flow_asset = layer.flow_asset_id.empty()
                    ? nullptr
                    : manifest.find_asset(layer.flow_asset_id);
                if (layer.placement != CharacterLayerPlacement::SourceCanvas ||
                    mask_asset == nullptr || layer.mesh_rows < 2 ||
                    layer.mesh_rows > 128 ||
                    !std::isfinite(layer.mesh_strength) ||
                    layer.mesh_strength < 0.0 || layer.mesh_strength > 4.0 ||
                    !std::isfinite(layer.idle_strength) ||
                    layer.idle_strength < 0.0 || layer.idle_strength > 32.0 ||
                    !std::isfinite(layer.idle_phase) ||
                    std::abs(layer.idle_phase) > 100.0 ||
                    !std::isfinite(layer.flow_strength) ||
                    layer.flow_strength < 0.0 || layer.flow_strength > 4.0 ||
                    !std::isfinite(layer.flow_frequency) ||
                    layer.flow_frequency < 0.0 || layer.flow_frequency > 10.0 ||
                    !std::isfinite(layer.flow_phase) ||
                    std::abs(layer.flow_phase) > 100.0 ||
                    (!layer.flow_asset_id.empty() && flow_asset == nullptr)) {
                    set_error(
                        error_message,
                        "Character mesh layer '" + layer.id + "' has invalid mesh settings"
                    );
                    return std::nullopt;
                }
                if (mask_asset->size.width != texture_asset->size.width ||
                    mask_asset->size.height != texture_asset->size.height ||
                    mask_asset->offset.x != texture_asset->offset.x ||
                    mask_asset->offset.y != texture_asset->offset.y) {
                    set_error(
                        error_message,
                        "Character mesh layer '" + layer.id +
                            "' texture and mask geometry disagree"
                    );
                    return std::nullopt;
                }
                if (flow_asset != nullptr &&
                    (flow_asset->size.width != texture_asset->size.width ||
                     flow_asset->size.height != texture_asset->size.height ||
                     flow_asset->offset.x != texture_asset->offset.x ||
                     flow_asset->offset.y != texture_asset->offset.y)) {
                    set_error(
                        error_message,
                        "Character mesh layer '" + layer.id +
                            "' texture and flow-map geometry disagree"
                    );
                    return std::nullopt;
                }
            }
            manifest.layers.push_back(std::move(layer));
        }

        if (rig_json->contains("expression")) {
            const auto& expression_json = rig_json->at("expression");
            CharacterExpressionRig expression;
            expression.enabled = true;
            expression.eyes_layer_id = expression_json.at("eyesLayer").get<std::string>();
            expression.mouth_layer_id = expression_json.at("mouthLayer").get<std::string>();
            expression.eyes_inward_asset_id =
                expression_json.at("eyes").at("inward").get<std::string>();
            expression.eyes_half_asset_id =
                expression_json.at("eyes").at("half").get<std::string>();
            expression.eyes_closed_asset_id =
                expression_json.at("eyes").at("closed").get<std::string>();
            expression.mouth_curious_asset_id =
                expression_json.at("mouth").at("curious").get<std::string>();

            const CharacterLayer* eyes_layer = manifest.find_layer(
                expression.eyes_layer_id
            );
            const CharacterLayer* mouth_layer = manifest.find_layer(
                expression.mouth_layer_id
            );
            if (eyes_layer == nullptr || mouth_layer == nullptr ||
                eyes_layer->renderer != CharacterLayerRenderer::Static ||
                mouth_layer->renderer != CharacterLayerRenderer::Static) {
                set_error(
                    error_message,
                    "Character expression rig references missing or non-static layers"
                );
                return std::nullopt;
            }

            const CharacterAsset* eyes_base = manifest.find_asset(
                eyes_layer->asset_id
            );
            const CharacterAsset* mouth_base = manifest.find_asset(
                mouth_layer->asset_id
            );
            const auto geometry_matches = [](
                const CharacterAsset* base,
                const CharacterAsset* variant
            ) {
                return base != nullptr && variant != nullptr &&
                    base->family == variant->family &&
                    base->size.width == variant->size.width &&
                    base->size.height == variant->size.height &&
                    base->offset.x == variant->offset.x &&
                    base->offset.y == variant->offset.y;
            };

            const bool eyes_valid =
                geometry_matches(
                    eyes_base,
                    manifest.find_asset(expression.eyes_inward_asset_id)
                ) &&
                geometry_matches(
                    eyes_base,
                    manifest.find_asset(expression.eyes_half_asset_id)
                ) &&
                geometry_matches(
                    eyes_base,
                    manifest.find_asset(expression.eyes_closed_asset_id)
                );
            const bool mouth_valid = geometry_matches(
                mouth_base,
                manifest.find_asset(expression.mouth_curious_asset_id)
            );
            if (!eyes_valid || !mouth_valid) {
                set_error(
                    error_message,
                    "Character expression variants must share their layer family geometry"
                );
                return std::nullopt;
            }

            manifest.expression = std::move(expression);
        }
    } catch (const std::exception& parse_error) {
        set_error(
            error_message,
            "Invalid character manifest contract: " + std::string(parse_error.what())
        );
        return std::nullopt;
    }

    if (manifest.layers.empty()) {
        set_error(error_message, "Character rig contains no layers");
        return std::nullopt;
    }
    if (!validate_family_geometry(*scale_json, manifest, error_message)) {
        return std::nullopt;
    }

    return manifest;
}

const CharacterAsset* CharacterManifest::find_asset(std::string_view id) const {
    const auto found = assets.find(std::string(id));
    return found == assets.end() ? nullptr : &found->second;
}

const CharacterLayer* CharacterManifest::find_layer(std::string_view id) const {
    const auto found = std::find_if(
        layers.begin(), layers.end(),
        [id](const CharacterLayer& layer) { return layer.id == id; }
    );
    return found == layers.end() ? nullptr : &*found;
}

} // namespace realmheart::animation::character
