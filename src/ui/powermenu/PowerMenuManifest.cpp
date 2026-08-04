#include "ui/powermenu/PowerMenuManifest.hpp"

#include "nlohmann_json/json.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace realmheart::ui::powermenu {
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
        set_error(error_message, "Unable to open power-menu manifest: " + path.string());
        return std::nullopt;
    }

    try {
        return Json::parse(input);
    } catch (const std::exception& error) {
        set_error(
            error_message,
            "Unable to parse power-menu manifest " + path.string() + ": " + error.what()
        );
        return std::nullopt;
    }
}

bool path_is_inside(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate
) {
    const auto mismatch = std::mismatch(
        root.begin(), root.end(), candidate.begin(), candidate.end()
    );
    return mismatch.first == root.end();
}

std::optional<PowerMenuCanvas> read_canvas(
    const Json& asset_json,
    std::string* error_message
) {
    try {
        const auto& value = asset_json.at("canvas").at("1x");
        PowerMenuCanvas canvas{
            .width = value.at(0).get<int>(),
            .height = value.at(1).get<int>(),
        };
        if (canvas.width <= 0 || canvas.height <= 0) {
            set_error(error_message, "Power-menu asset has an invalid 1x canvas");
            return std::nullopt;
        }
        return canvas;
    } catch (const std::exception& error) {
        set_error(
            error_message,
            "Invalid power-menu 1x canvas: " + std::string(error.what())
        );
        return std::nullopt;
    }
}

std::optional<PowerMenuPlacement> read_placement(
    const Json& asset_json,
    std::string* error_message
) {
    try {
        const auto& value = asset_json.at("placement").at("1x");
        PowerMenuPlacement placement{
            .x = value.at("x").get<int>(),
            .y = value.at("y").get<int>(),
            .width = value.at("width").get<int>(),
            .height = value.at("height").get<int>(),
        };
        if (placement.width <= 0 || placement.height <= 0) {
            set_error(error_message, "Power-menu asset has an invalid 1x placement size");
            return std::nullopt;
        }
        return placement;
    } catch (const std::exception& error) {
        set_error(
            error_message,
            "Invalid power-menu 1x placement: " + std::string(error.what())
        );
        return std::nullopt;
    }
}

std::optional<PowerMenuVector> read_vector(
    const Json& value,
    std::string_view field,
    std::string* error_message
) {
    try {
        if (!value.is_array() || value.size() != 2U) {
            set_error(error_message, "Power-menu rig field '" + std::string(field) +
                "' must contain exactly two numbers");
            return std::nullopt;
        }
        PowerMenuVector vector{
            .x = value.at(0).get<double>(),
            .y = value.at(1).get<double>(),
        };
        if (!std::isfinite(vector.x) || !std::isfinite(vector.y)) {
            set_error(error_message, "Power-menu rig field '" + std::string(field) +
                "' contains a non-finite value");
            return std::nullopt;
        }
        return vector;
    } catch (const std::exception& error) {
        set_error(error_message, "Invalid power-menu rig vector '" +
            std::string(field) + "': " + error.what());
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> resolve_rig_map(
    const std::filesystem::path& root,
    const std::string& relative_text,
    std::string_view field,
    std::string* error_message
) {
    const std::filesystem::path relative(relative_text);
    if (relative.empty() || relative.is_absolute()) {
        set_error(error_message, "Power-menu rig field '" + std::string(field) +
            "' must be a relative 1x map path");
        return std::nullopt;
    }

    std::error_code error;
    const auto one_x_root = std::filesystem::weakly_canonical(root / "1x", error);
    error.clear();
    const auto candidate = std::filesystem::weakly_canonical(one_x_root / relative, error);
    if (error || !path_is_inside(one_x_root, candidate) ||
        !std::filesystem::is_regular_file(candidate, error) || error) {
        set_error(error_message, "Power-menu rig map is missing or escapes its root: " +
            relative_text);
        return std::nullopt;
    }
    return candidate;
}

std::optional<PowerMenuAnimationType> read_animation_type(
    std::string_view value,
    std::string* error_message
) {
    if (value == "static") return PowerMenuAnimationType::Static;
    if (value == "drift") return PowerMenuAnimationType::Drift;
    if (value == "flow-drift") return PowerMenuAnimationType::FlowDrift;
    if (value == "spring") return PowerMenuAnimationType::Spring;
    if (value == "mesh-flow") return PowerMenuAnimationType::MeshFlow;
    if (value == "blink-patch") return PowerMenuAnimationType::BlinkPatch;
    if (value == "glow-mask") return PowerMenuAnimationType::GlowMask;
    set_error(error_message, "Unsupported power-menu animation type: " +
        std::string(value));
    return std::nullopt;
}

bool read_mesh_flow_animation(
    const Json& value,
    const std::filesystem::path& root,
    PowerMenuAnimationRig& animation,
    std::string* error_message
) {
    try {
        animation.movement_mask = value.at("movementMask").get<std::string>();
        animation.flow_map = value.at("flowMap").get<std::string>();
        const auto movement = resolve_rig_map(
            root, animation.movement_mask, "movementMask", error_message
        );
        const auto flow_map = resolve_rig_map(
            root, animation.flow_map, "flowMap", error_message
        );
        if (!movement || !flow_map) return false;
        animation.movement_mask_path = *movement;
        animation.flow_map_path = *flow_map;

        const auto& mesh = value.at("mesh");
        const std::string axis = mesh.at("stripAxis").get<std::string>();
        if (axis == "rows") animation.mesh.strip_axis = PowerMenuStripAxis::Rows;
        else if (axis == "columns") animation.mesh.strip_axis = PowerMenuStripAxis::Columns;
        else {
            set_error(error_message, "Unsupported power-menu mesh stripAxis: " + axis);
            return false;
        }
        const std::string anchor = mesh.at("anchorMode").get<std::string>();
        if (anchor == "pinned-minimum") {
            animation.mesh.anchor_mode = PowerMenuAnchorMode::PinnedMinimum;
        } else if (anchor == "pinned-maximum") {
            animation.mesh.anchor_mode = PowerMenuAnchorMode::PinnedMaximum;
        } else if (anchor == "weighted-translate") {
            animation.mesh.anchor_mode = PowerMenuAnchorMode::WeightedTranslate;
        } else {
            set_error(error_message, "Unsupported power-menu mesh anchorMode: " + anchor);
            return false;
        }
        const auto direction = read_vector(
            mesh.at("direction"), "mesh.direction", error_message
        );
        if (!direction) return false;
        animation.mesh.direction = *direction;
        animation.mesh.strip_count = mesh.at("stripCount").get<int>();
        animation.mesh.amplitude = mesh.at("amplitude").get<double>();

        const auto& flow = value.at("flow");
        animation.flow.pose_count = flow.at("poseCount").get<int>();
        animation.flow.amplitude = flow.at("amplitude").get<double>();
        animation.flow.frequency = flow.at("frequency").get<double>();
        animation.flow.phase = flow.at("phase").get<double>();
    } catch (const std::exception& error) {
        set_error(error_message, "Invalid mesh-flow power-menu animation: " +
            std::string(error.what()));
        return false;
    }

    if (animation.mesh.strip_count <= 0 || animation.mesh.strip_count > 64 ||
        std::hypot(animation.mesh.direction.x, animation.mesh.direction.y) <= 0.0 ||
        !std::isfinite(animation.mesh.amplitude) || animation.mesh.amplitude < 0.0 ||
        animation.flow.pose_count < 2 || animation.flow.pose_count > 5 ||
        !std::isfinite(animation.flow.amplitude) || animation.flow.amplitude < 0.0 ||
        !std::isfinite(animation.flow.frequency) || animation.flow.frequency <= 0.0 ||
        !std::isfinite(animation.flow.phase)) {
        set_error(error_message, "Mesh-flow power-menu animation has invalid bounds");
        return false;
    }
    return true;
}

bool read_periodic_fields(
    const Json& value,
    PowerMenuAnimationRig& animation,
    std::string* error_message
) {
    try {
        animation.frequency = value.at("frequency").get<double>();
        animation.phase = value.at("phase").get<double>();
    } catch (const std::exception& error) {
        set_error(error_message, "Periodic power-menu animation is incomplete: " +
            std::string(error.what()));
        return false;
    }
    if (!std::isfinite(animation.frequency) || animation.frequency <= 0.0 ||
        !std::isfinite(animation.phase)) {
        set_error(error_message,
            "Power-menu animation frequency must be positive and finite");
        return false;
    }
    return true;
}

bool read_flow_map_fields(
    const Json& value,
    const std::filesystem::path& root,
    PowerMenuAnimationRig& animation,
    std::string* error_message
) {
    try {
        animation.movement_mask = value.at("movementMask").get<std::string>();
        animation.flow_map = value.at("flowMap").get<std::string>();
    } catch (const std::exception& error) {
        set_error(error_message, "Flow animation is missing movementMask or flowMap: " +
            std::string(error.what()));
        return false;
    }
    const auto movement = resolve_rig_map(
        root, animation.movement_mask, "movementMask", error_message
    );
    const auto flow = resolve_rig_map(root, animation.flow_map, "flowMap", error_message);
    if (!movement || !flow) return false;
    animation.movement_mask_path = *movement;
    animation.flow_map_path = *flow;
    return true;
}

bool read_non_mesh_animation(
    const Json& value,
    const std::filesystem::path& root,
    PowerMenuAnimationRig& animation,
    std::string* error_message
) {
    try {
        switch (animation.type) {
        case PowerMenuAnimationType::Static:
            return true;
        case PowerMenuAnimationType::Drift: {
            const auto translation = read_vector(
                value.at("translation"), "translation", error_message
            );
            if (!translation || !read_periodic_fields(value, animation, error_message)) {
                return false;
            }
            animation.translation = *translation;
            animation.opacity_amplitude = value.at("opacityAmplitude").get<double>();
            if (!std::isfinite(animation.opacity_amplitude) ||
                animation.opacity_amplitude < 0.0 || animation.opacity_amplitude > 1.0) {
                set_error(error_message, "Drift opacityAmplitude must be between 0 and 1");
                return false;
            }
            return true;
        }
        case PowerMenuAnimationType::FlowDrift: {
            if (!read_flow_map_fields(value, root, animation, error_message)) return false;
            const auto translation = read_vector(
                value.at("translation"), "translation", error_message
            );
            if (!translation || !read_periodic_fields(value, animation, error_message)) {
                return false;
            }
            animation.translation = *translation;
            animation.flow.pose_count = value.at("poseCount").get<int>();
            animation.scale_amplitude = value.at("scaleAmplitude").get<double>();
            animation.opacity_amplitude = value.at("opacityAmplitude").get<double>();
            if (animation.flow.pose_count < 2 || animation.flow.pose_count > 5 ||
                !std::isfinite(animation.scale_amplitude) ||
                animation.scale_amplitude < 0.0 ||
                !std::isfinite(animation.opacity_amplitude) ||
                animation.opacity_amplitude < 0.0 || animation.opacity_amplitude > 1.0) {
                set_error(error_message, "Flow-drift power-menu animation has invalid bounds");
                return false;
            }
            return true;
        }
        case PowerMenuAnimationType::Spring: {
            const auto pivot = read_vector(value.at("pivot"), "pivot", error_message);
            const auto translation = read_vector(
                value.at("translation"), "translation", error_message
            );
            if (!pivot || !translation ||
                !read_periodic_fields(value, animation, error_message)) return false;
            animation.pivot = *pivot;
            animation.translation = *translation;
            animation.rotation_degrees = value.at("rotationDegrees").get<double>();
            animation.damping = value.at("damping").get<double>();
            if (animation.pivot.x < 0.0 || animation.pivot.x > 1.0 ||
                animation.pivot.y < 0.0 || animation.pivot.y > 1.0 ||
                !std::isfinite(animation.rotation_degrees) ||
                !std::isfinite(animation.damping) || animation.damping < 0.0 ||
                animation.damping > 1.0) {
                set_error(error_message, "Spring power-menu animation has invalid bounds");
                return false;
            }
            return true;
        }
        case PowerMenuAnimationType::BlinkPatch:
            animation.blink_state = value.at("state").get<std::string>();
            if (animation.blink_state != "half" && animation.blink_state != "closed") {
                set_error(error_message, "Blink patch state must be 'half' or 'closed'");
                return false;
            }
            return true;
        case PowerMenuAnimationType::GlowMask:
            animation.tint_role = value.at("tintRole").get<std::string>();
            animation.idle_minimum = value.at("idleMinimum").get<double>();
            animation.idle_maximum = value.at("idleMaximum").get<double>();
            animation.frequency = value.value("frequency", 0.16);
            if (animation.tint_role.empty() || !std::isfinite(animation.idle_minimum) ||
                !std::isfinite(animation.idle_maximum) || animation.idle_minimum < 0.0 ||
                animation.idle_minimum > animation.idle_maximum ||
                animation.idle_maximum > 1.0 || !std::isfinite(animation.frequency) ||
                animation.frequency <= 0.0 || animation.frequency > 1.0) {
                set_error(error_message, "Glow-mask power-menu animation has invalid bounds");
                return false;
            }
            return true;
        case PowerMenuAnimationType::MeshFlow:
            return read_mesh_flow_animation(value, root, animation, error_message);
        }
    } catch (const std::exception& error) {
        set_error(error_message, "Invalid power-menu animation fields: " +
            std::string(error.what()));
        return false;
    }
    return false;
}

} // namespace

std::optional<PowerMenuManifest> PowerMenuManifest::load(
    const std::filesystem::path& manifest_path,
    std::string* error_message
) {
    std::error_code error;
    const auto canonical_manifest = std::filesystem::weakly_canonical(
        manifest_path,
        error
    );
    if (error || !std::filesystem::is_regular_file(canonical_manifest, error) || error) {
        set_error(
            error_message,
            "Power-menu manifest does not exist: " + manifest_path.string()
        );
        return std::nullopt;
    }

    const auto manifest_json = read_json(canonical_manifest, error_message);
    if (!manifest_json) return std::nullopt;

    PowerMenuManifest manifest;
    manifest.root = canonical_manifest.parent_path();
    std::unordered_set<std::string> asset_names;

    try {
        const auto& assets_json = manifest_json->at("assets");
        if (!assets_json.is_array() || assets_json.empty()) {
            set_error(error_message, "Power-menu manifest contains no assets");
            return std::nullopt;
        }

        manifest.assets.reserve(assets_json.size());
        for (const auto& asset_json : assets_json) {
            PowerMenuAsset asset;
            asset.name = asset_json.at("name").get<std::string>();
            asset.category = asset_json.at("category").get<std::string>();
            asset.file_1x = asset_json.at("files").at("1x").get<std::string>();

            if (asset.name.empty() || !asset_names.insert(asset.name).second) {
                set_error(
                    error_message,
                    "Power-menu asset name is empty or duplicated: " + asset.name
                );
                return std::nullopt;
            }
            if (asset.category.empty() || asset.file_1x.empty()) {
                set_error(
                    error_message,
                    "Power-menu asset '" + asset.name + "' has an empty category or 1x file"
                );
                return std::nullopt;
            }

            const std::filesystem::path relative_path(asset.file_1x);
            if (relative_path.is_absolute()) {
                set_error(
                    error_message,
                    "Power-menu asset uses an absolute path: " + asset.file_1x
                );
                return std::nullopt;
            }

            error.clear();
            const auto canonical_asset = std::filesystem::weakly_canonical(
                manifest.root / relative_path,
                error
            );
            if (error || !path_is_inside(manifest.root, canonical_asset) ||
                !std::filesystem::is_regular_file(canonical_asset, error) || error) {
                set_error(
                    error_message,
                    "Power-menu asset file is missing or escapes its root: " +
                        asset.file_1x
                );
                return std::nullopt;
            }
            asset.path_1x = canonical_asset;

            const auto canvas = read_canvas(asset_json, error_message);
            const auto placement = read_placement(asset_json, error_message);
            if (!canvas || !placement) return std::nullopt;
            asset.canvas_1x = *canvas;
            asset.placement_1x = *placement;

            manifest.assets.push_back(std::move(asset));
        }
    } catch (const std::exception& exception) {
        set_error(
            error_message,
            "Invalid power-menu manifest structure: " + std::string(exception.what())
        );
        return std::nullopt;
    }

    const PowerMenuAsset* static_base = manifest.find_asset("static-base");
    if (static_base == nullptr || static_base->placement_1x.x != 0 ||
        static_base->placement_1x.y != 0 ||
        static_base->placement_1x.width != static_base->canvas_1x.width ||
        static_base->placement_1x.height != static_base->canvas_1x.height) {
        set_error(
            error_message,
            "Power-menu manifest requires a full-canvas static-base asset"
        );
        return std::nullopt;
    }
    manifest.logical_canvas = static_base->canvas_1x;

    for (const auto& asset : manifest.assets) {
        if (asset.canvas_1x.width != manifest.logical_canvas.width ||
            asset.canvas_1x.height != manifest.logical_canvas.height) {
            set_error(
                error_message,
                "Power-menu asset '" + asset.name + "' disagrees with the logical canvas"
            );
            return std::nullopt;
        }
    }

    return manifest;
}

const PowerMenuAsset* PowerMenuManifest::find_asset(std::string_view name) const {
    const auto found = std::find_if(
        assets.begin(),
        assets.end(),
        [name](const PowerMenuAsset& asset) { return asset.name == name; }
    );
    return found == assets.end() ? nullptr : &*found;
}

std::optional<PowerMenuRig> PowerMenuRig::load(
    const std::filesystem::path& rig_path,
    const PowerMenuManifest& manifest,
    std::string* error_message
) {
    std::error_code error;
    const auto canonical_rig = std::filesystem::weakly_canonical(rig_path, error);
    if (error || !std::filesystem::is_regular_file(canonical_rig, error) || error) {
        set_error(error_message, "Power-menu rig does not exist: " + rig_path.string());
        return std::nullopt;
    }
    const auto rig_json = read_json(canonical_rig, error_message);
    if (!rig_json) return std::nullopt;

    PowerMenuRig rig;
    rig.root = canonical_rig.parent_path();
    std::unordered_set<std::string> asset_ids;
    std::unordered_set<int> z_values;

    try {
        const auto& canvas = rig_json->at("logicalCanvas");
        rig.logical_canvas = {
            .width = canvas.at(0).get<int>(),
            .height = canvas.at(1).get<int>(),
        };
        if (rig.logical_canvas.width != 1200 || rig.logical_canvas.height != 675 ||
            rig.logical_canvas.width != manifest.logical_canvas.width ||
            rig.logical_canvas.height != manifest.logical_canvas.height) {
            set_error(error_message,
                "Power-menu rig logicalCanvas must match the 1200x675 manifest canvas");
            return std::nullopt;
        }

        const auto& layers = rig_json->at("layers");
        if (!layers.is_array() || layers.empty()) {
            set_error(error_message, "Power-menu rig contains no layers");
            return std::nullopt;
        }
        rig.layers.reserve(layers.size());
        for (const auto& layer_json : layers) {
            PowerMenuRigLayer layer;
            layer.asset = layer_json.at("asset").get<std::string>();
            layer.z = layer_json.at("z").get<int>();
            if (layer.asset.empty() || !asset_ids.insert(layer.asset).second) {
                set_error(error_message,
                    "Power-menu rig asset is empty or duplicated: " + layer.asset);
                return std::nullopt;
            }
            if (!z_values.insert(layer.z).second) {
                set_error(error_message,
                    "Power-menu rig z-order value is duplicated: " +
                        std::to_string(layer.z));
                return std::nullopt;
            }
            if (manifest.find_asset(layer.asset) == nullptr) {
                set_error(error_message,
                    "Power-menu rig references missing manifest asset: " + layer.asset);
                return std::nullopt;
            }

            const auto& animation_json = layer_json.at("animation");
            const auto type = read_animation_type(
                animation_json.at("type").get<std::string>(), error_message
            );
            if (!type) return std::nullopt;
            layer.animation.type = *type;
            if (!read_non_mesh_animation(
                    animation_json, rig.root, layer.animation, error_message)) {
                return std::nullopt;
            }
            rig.layers.push_back(std::move(layer));
        }

        const auto& blink = rig_json->at("blink");
        rig.blink.half_asset = blink.at("halfAsset").get<std::string>();
        rig.blink.closed_asset = blink.at("closedAsset").get<std::string>();
        rig.blink.minimum_interval_seconds =
            blink.at("minimumIntervalSeconds").get<double>();
        rig.blink.maximum_interval_seconds =
            blink.at("maximumIntervalSeconds").get<double>();
        rig.blink.double_blink_chance = blink.at("doubleBlinkChance").get<double>();
        rig.blink.duration_seconds = blink.at("durationSeconds").get<double>();
        if (manifest.find_asset(rig.blink.half_asset) == nullptr ||
            manifest.find_asset(rig.blink.closed_asset) == nullptr ||
            !std::isfinite(rig.blink.minimum_interval_seconds) ||
            !std::isfinite(rig.blink.maximum_interval_seconds) ||
            rig.blink.minimum_interval_seconds <= 0.0 ||
            rig.blink.minimum_interval_seconds > rig.blink.maximum_interval_seconds ||
            !std::isfinite(rig.blink.double_blink_chance) ||
            rig.blink.double_blink_chance < 0.0 ||
            rig.blink.double_blink_chance > 1.0 ||
            !std::isfinite(rig.blink.duration_seconds) ||
            rig.blink.duration_seconds <= 0.0 ||
            rig.blink.duration_seconds > 2.0) {
            set_error(error_message, "Power-menu rig blink configuration is invalid");
            return std::nullopt;
        }
    } catch (const std::exception& exception) {
        set_error(error_message, "Invalid power-menu rig structure: " +
            std::string(exception.what()));
        return std::nullopt;
    }

    std::sort(rig.layers.begin(), rig.layers.end(),
        [](const PowerMenuRigLayer& left, const PowerMenuRigLayer& right) {
            return left.z < right.z;
        });
    return rig;
}

const PowerMenuRigLayer* PowerMenuRig::find_layer(std::string_view asset) const {
    const auto found = std::find_if(
        layers.begin(), layers.end(),
        [asset](const PowerMenuRigLayer& layer) { return layer.asset == asset; }
    );
    return found == layers.end() ? nullptr : &*found;
}

} // namespace realmheart::ui::powermenu
