#include "animation/character/CharacterManifest.hpp"

#include "nlohmann_json/json.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace realmheart::animation::character {
namespace {

using Json = nlohmann::json;

void set_error(std::string* target, std::string message) {
    if (target != nullptr) *target = std::move(message);
}

bool path_is_within(
    const std::filesystem::path& parent,
    const std::filesystem::path& candidate
) {
    auto parent_it = parent.begin();
    auto candidate_it = candidate.begin();
    for (; parent_it != parent.end() && candidate_it != candidate.end();
         ++parent_it, ++candidate_it) {
        if (*parent_it != *candidate_it) return false;
    }
    return parent_it == parent.end();
}

bool checked_pixel_count(
    int width,
    int height,
    std::uint64_t* output
) noexcept {
    if (width <= 0 || height <= 0) return false;
    const auto unsigned_width = static_cast<std::uint64_t>(width);
    const auto unsigned_height = static_cast<std::uint64_t>(height);
    if (unsigned_width > CharacterResourceBudget::kMaxSourceDimension ||
        unsigned_height > CharacterResourceBudget::kMaxSourceDimension ||
        unsigned_width > CharacterResourceBudget::kMaxSourcePixels / unsigned_height) {
        return false;
    }
    if (output != nullptr) *output = unsigned_width * unsigned_height;
    return true;
}

std::string optional_string(const Json& parent, std::string_view key) noexcept {
    try {
        return parent.value(std::string(key), std::string{});
    } catch (const std::exception&) {
        return {};
    }
}

template <typename T>
std::optional<T> optional_value(const Json& parent, std::string_view key) noexcept {
    try {
        if (!parent.contains(std::string(key))) return std::nullopt;
        return parent.at(std::string(key)).get<T>();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<Json> read_json(
    const std::filesystem::path& path,
    std::string* error_message
) {
    std::error_code file_error;
    const auto file_bytes = std::filesystem::file_size(path, file_error);
    if (file_error || file_bytes > CharacterResourceBudget::kMaxManifestFileBytes) {
        set_error(error_message, "Character manifest exceeds its file-size resource budget: " + path.string());
        return std::nullopt;
    }
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

std::optional<Json> parse_opened_json(
    std::string_view contents,
    const std::filesystem::path& path,
    std::string* error_message
) {
    if (contents.size() > CharacterResourceBudget::kMaxManifestFileBytes) {
        set_error(error_message, "Character manifest exceeds its file-size resource budget: " + path.string());
        return std::nullopt;
    }
    try {
        return Json::parse(contents);
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
                    // Expression, mask, and flow members are optional. Their
                    // owning layer will select a lower-cost fallback below.
                    continue;
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
    std::string* error_message,
    std::string_view opened_rig_contents
) {
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(character_root, error);
    if (error || !std::filesystem::is_directory(canonical_root, error) || error) {
        set_error(error_message, "Character asset root does not exist: " + character_root.string());
        return std::nullopt;
    }

    const auto rig_path = canonical_root / "rig.json";
    const auto rig_json = opened_rig_contents.empty()
        ? read_json(rig_path, error_message)
        : parse_opened_json(opened_rig_contents, rig_path, error_message);
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
            manifest.source_canvas.height <= 0 ||
            !checked_pixel_count(
                manifest.source_canvas.width,
                manifest.source_canvas.height,
                nullptr
            )) {
            set_error(error_message, "Character manifest exceeds its resource budget or has an invalid identity/source canvas");
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
        const auto canonical_scale_directory = std::filesystem::weakly_canonical(
            scale_directory, error
        );
        if (error || !std::filesystem::is_directory(canonical_scale_directory, error) || error) {
            set_error(error_message, "Selected character display-tier directory is invalid");
            return std::nullopt;
        }

        std::unordered_set<std::string> optional_asset_ids;
        std::unordered_set<std::string> required_asset_ids;
        for (const auto& layer_json : rig_json->at("layers")) {
            required_asset_ids.insert(layer_json.at("asset").get<std::string>());
            const auto mask_id = optional_string(layer_json, "mask");
            const auto flow_id = optional_string(layer_json, "flow");
            if (!mask_id.empty()) optional_asset_ids.insert(mask_id);
            if (!flow_id.empty()) optional_asset_ids.insert(flow_id);
        }
        if (rig_json->contains("expression")) {
            try {
                const auto& expression_json = rig_json->at("expression");
                for (const auto& id : {
                    optional_string(expression_json.at("eyes"), "inward"),
                    optional_string(expression_json.at("eyes"), "half"),
                    optional_string(expression_json.at("eyes"), "closed"),
                    optional_string(expression_json.at("mouth"), "curious"),
                }) {
                    if (!id.empty()) optional_asset_ids.insert(id);
                }
            } catch (const std::exception&) {
                // A malformed optional expression must not make the baseline
                // character package unloadable.
            }
        }

        std::uint64_t decoded_bytes = 0U;
        for (const auto& [asset_id, asset_json] : scale_json->at("assets").items()) {
            const bool optional = optional_asset_ids.contains(asset_id) &&
                !required_asset_ids.contains(asset_id);
            CharacterAsset asset;
            try {
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
            } catch (const std::exception&) {
                if (optional) continue;
                throw;
            }

            std::uint64_t asset_pixels = 0U;
            if (asset.id.empty() || asset.file.empty() || asset.family.empty() ||
                !checked_pixel_count(asset.size.width, asset.size.height, &asset_pixels) ||
                !std::isfinite(asset.offset.x) || !std::isfinite(asset.offset.y)) {
                if (optional) continue;
                set_error(error_message, "Character asset '" + asset_id + "' exceeds its resource budget or is invalid");
                return std::nullopt;
            }
            const auto canonical_asset = std::filesystem::weakly_canonical(asset.path, error);
            if (error) {
                if (optional) continue;
                set_error(error_message, "Unable to canonicalize character asset: " + asset.path.string());
                return std::nullopt;
            }
            if (!path_is_within(canonical_scale_directory, canonical_asset)) {
                set_error(error_message, "Character asset escapes its selected display tier: " + asset.path.string());
                return std::nullopt;
            }
            if (!path_is_within(canonical_root, canonical_asset)) {
                set_error(error_message, "Character asset escapes its asset root: " + asset.path.string());
                return std::nullopt;
            }
            if (!std::filesystem::is_regular_file(canonical_asset, error) || error) {
                if (optional) continue;
                set_error(error_message, "Character asset file is missing: " + asset.path.string());
                return std::nullopt;
            }
            const auto file_bytes = std::filesystem::file_size(canonical_asset, error);
            if (error || file_bytes > CharacterResourceBudget::kMaxAssetFileBytes) {
                if (optional) continue;
                set_error(error_message, "Character asset exceeds its file-size resource budget: " + asset.path.string());
                return std::nullopt;
            }
            if (asset_pixels > (std::numeric_limits<std::uint64_t>::max() - decoded_bytes) / 4U ||
                decoded_bytes + (asset_pixels * 4U) > CharacterResourceBudget::kMaxDecodedBytes) {
                if (optional) continue;
                set_error(error_message, "Character assets exceed their resource budget");
                return std::nullopt;
            }
            decoded_bytes += asset_pixels * 4U;
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
                layer.mask_asset_id = optional_string(layer_json, "mask");
                layer.flow_asset_id = optional_string(layer_json, "flow");
                const auto mesh_rows = optional_value<int>(layer_json, "meshRows");
                const auto mesh_strength = optional_value<double>(layer_json, "meshStrength");
                const auto idle_strength = optional_value<double>(layer_json, "idleStrength");
                const auto idle_phase = optional_value<double>(layer_json, "idlePhase");
                const auto flow_strength = optional_value<double>(layer_json, "flowStrength");
                const auto flow_frequency = optional_value<double>(layer_json, "flowFrequency");
                const auto flow_phase = optional_value<double>(layer_json, "flowPhase");
                layer.mesh_rows = mesh_rows.value_or(24);
                layer.mesh_strength = mesh_strength.value_or(1.0);
                layer.idle_strength = idle_strength.value_or(0.0);
                layer.idle_phase = idle_phase.value_or(0.0);
                layer.flow_strength = flow_strength.value_or(0.0);
                layer.flow_frequency = flow_frequency.value_or(0.0);
                layer.flow_phase = flow_phase.value_or(0.0);

                const CharacterAsset* mask_asset = manifest.find_asset(
                    layer.mask_asset_id
                );
                const CharacterAsset* flow_asset = layer.flow_asset_id.empty()
                    ? nullptr
                    : manifest.find_asset(layer.flow_asset_id);
                const bool mesh_settings_valid =
                    (!layer_json.contains("meshRows") || mesh_rows.has_value()) &&
                    (!layer_json.contains("meshStrength") || mesh_strength.has_value()) &&
                    (!layer_json.contains("idleStrength") || idle_strength.has_value()) &&
                    (!layer_json.contains("idlePhase") || idle_phase.has_value()) &&
                    layer.placement == CharacterLayerPlacement::SourceCanvas &&
                    mask_asset != nullptr && layer.mesh_rows >= 2 &&
                    layer.mesh_rows <= CharacterResourceBudget::kMaxMeshRows &&
                    std::isfinite(layer.mesh_strength) &&
                    layer.mesh_strength >= 0.0 && layer.mesh_strength <= 4.0 &&
                    std::isfinite(layer.idle_strength) &&
                    layer.idle_strength >= 0.0 && layer.idle_strength <= 32.0 &&
                    std::isfinite(layer.idle_phase) &&
                    std::abs(layer.idle_phase) <= 100.0 &&
                    std::isfinite(layer.flow_strength) &&
                    layer.flow_strength >= 0.0 && layer.flow_strength <= 4.0 &&
                    std::isfinite(layer.flow_frequency) &&
                    layer.flow_frequency >= 0.0 && layer.flow_frequency <= 10.0 &&
                    std::isfinite(layer.flow_phase) &&
                    std::abs(layer.flow_phase) <= 100.0;
                const bool flow_settings_valid =
                    (!layer_json.contains("flowStrength") || flow_strength.has_value()) &&
                    (!layer_json.contains("flowFrequency") || flow_frequency.has_value()) &&
                    (!layer_json.contains("flowPhase") || flow_phase.has_value());
                if (!mesh_settings_valid) {
                    layer.mesh_available = false;
                    layer.mask_asset_id.clear();
                    layer.flow_asset_id.clear();
                    layer.flow_strength = 0.0;
                } else if (mask_asset->size.width != texture_asset->size.width ||
                    mask_asset->size.height != texture_asset->size.height ||
                    mask_asset->offset.x != texture_asset->offset.x ||
                    mask_asset->offset.y != texture_asset->offset.y) {
                    layer.mesh_available = false;
                    layer.mask_asset_id.clear();
                    layer.flow_asset_id.clear();
                    layer.flow_strength = 0.0;
                } else if (!flow_settings_valid || (!layer.flow_asset_id.empty() &&
                    (flow_asset == nullptr ||
                     flow_asset->size.width != texture_asset->size.width ||
                     flow_asset->size.height != texture_asset->size.height ||
                     flow_asset->offset.x != texture_asset->offset.x ||
                     flow_asset->offset.y != texture_asset->offset.y))) {
                    layer.flow_asset_id.clear();
                    layer.flow_strength = 0.0;
                }
            }
            manifest.layers.push_back(std::move(layer));
        }

        if (rig_json->contains("expression")) {
            try {
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
                manifest.expression = {};
            } else {
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
                    manifest.expression = {};
                } else {
                    manifest.expression = std::move(expression);
                }
            }
        } catch (const std::exception&) {
            manifest.expression = {};
        }
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
