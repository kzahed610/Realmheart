#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realmheart::ui::powermenu {

struct PowerMenuCanvas {
    int width = 0;
    int height = 0;
};

struct PowerMenuPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct PowerMenuAsset {
    std::string name;
    std::string category;
    std::string file_1x;
    std::filesystem::path path_1x;
    PowerMenuCanvas canvas_1x;
    PowerMenuPlacement placement_1x;
};

enum class PowerMenuAnimationType {
    Static,
    Drift,
    FlowDrift,
    Spring,
    MeshFlow,
    BlinkPatch,
    GlowMask,
};

enum class PowerMenuStripAxis { Rows, Columns };

enum class PowerMenuAnchorMode {
    PinnedMinimum,
    PinnedMaximum,
    WeightedTranslate,
};

struct PowerMenuVector {
    double x = 0.0;
    double y = 0.0;
};

struct PowerMenuMeshRig {
    PowerMenuStripAxis strip_axis = PowerMenuStripAxis::Rows;
    int strip_count = 0;
    PowerMenuAnchorMode anchor_mode = PowerMenuAnchorMode::WeightedTranslate;
    PowerMenuVector direction;
    double amplitude = 0.0;
};

struct PowerMenuFlowRig {
    int pose_count = 0;
    double amplitude = 0.0;
    double frequency = 0.0;
    double phase = 0.0;
};

struct PowerMenuAnimationRig {
    PowerMenuAnimationType type = PowerMenuAnimationType::Static;
    std::string movement_mask;
    std::string flow_map;
    std::filesystem::path movement_mask_path;
    std::filesystem::path flow_map_path;
    PowerMenuMeshRig mesh;
    PowerMenuFlowRig flow;
    PowerMenuVector pivot;
    double rotation_degrees = 0.0;
    PowerMenuVector translation;
    double scale_amplitude = 0.0;
    double opacity_amplitude = 0.0;
    double frequency = 0.0;
    double damping = 0.0;
    double phase = 0.0;
    std::string blink_state;
    std::string tint_role;
    double idle_minimum = 0.0;
    double idle_maximum = 0.0;
};

struct PowerMenuRigLayer {
    std::string asset;
    int z = 0;
    PowerMenuAnimationRig animation;
};

struct PowerMenuBlinkRig {
    std::string half_asset;
    std::string closed_asset;
    double minimum_interval_seconds = 0.0;
    double maximum_interval_seconds = 0.0;
    double double_blink_chance = 0.0;
    double duration_seconds = 0.0;
};

class PowerMenuManifest {
public:
    static std::optional<PowerMenuManifest> load(
        const std::filesystem::path& manifest_path,
        std::string* error_message = nullptr
    );

    [[nodiscard]] const PowerMenuAsset* find_asset(std::string_view name) const;

    std::filesystem::path root;
    PowerMenuCanvas logical_canvas;
    std::vector<PowerMenuAsset> assets;
};

class PowerMenuRig {
public:
    static std::optional<PowerMenuRig> load(
        const std::filesystem::path& rig_path,
        const PowerMenuManifest& manifest,
        std::string* error_message = nullptr
    );

    [[nodiscard]] const PowerMenuRigLayer* find_layer(std::string_view asset) const;

    std::filesystem::path root;
    PowerMenuCanvas logical_canvas;
    std::vector<PowerMenuRigLayer> layers;
    PowerMenuBlinkRig blink;
};

} // namespace realmheart::ui::powermenu
