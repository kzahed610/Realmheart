#include "ui/powermenu/PowerMenuManifest.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

using realmheart::ui::powermenu::PowerMenuAnimationType;
using realmheart::ui::powermenu::PowerMenuManifest;
using realmheart::ui::powermenu::PowerMenuRig;

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path() /
            ("realmheart-power-menu-manifest-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << contents;
    assert(output.good());
}

std::string valid_manifest(std::string_view second_file = "1x/arthur/hair.png") {
    return std::string{R"JSON({
  "version": 1,
  "assets": [
    {
      "name": "static-base",
      "category": "base",
      "files": {"1x": "1x/base/static-base.png"},
      "canvas": {"1x": [1200, 675]},
      "placement": {"1x": {"x": 0, "y": 0, "width": 1200, "height": 675}}
    },
    {
      "name": "arthur-hair-back",
      "category": "arthur",
      "files": {"1x": ")JSON"} + std::string(second_file) + R"JSON("},
      "canvas": {"1x": [1200, 675]},
      "placement": {"1x": {"x": 312, "y": -3, "width": 190, "height": 252}}
    }
  ]
})JSON";
}

std::string valid_rig() {
    return R"JSON({
  "version": 1,
  "scene": "power-menu",
  "logicalCanvas": [1200, 675],
  "layers": [
    {"asset": "static-base", "z": 0, "animation": {"type": "static"}},
    {
      "asset": "arthur-hair-back",
      "z": 1,
      "animation": {
        "type": "mesh-flow",
        "movementMask": "rig/hair-mask.png",
        "flowMap": "rig/hair-flow.png",
        "mesh": {
          "stripAxis": "rows",
          "stripCount": 20,
          "anchorMode": "weighted-translate",
          "direction": [-0.95, 0.30],
          "amplitude": 4.0
        },
        "flow": {"poseCount": 5, "amplitude": 1.3, "frequency": 0.18, "phase": 0.0}
      }
    }
  ],
  "blink": {
    "halfAsset": "arthur-hair-back",
    "closedAsset": "static-base",
    "minimumIntervalSeconds": 3.5,
    "maximumIntervalSeconds": 8.0,
    "doubleBlinkChance": 0.12,
    "durationSeconds": 0.7
  }
})JSON";
}

void loads_valid_rig_and_resolves_map_paths() {
    TemporaryDirectory temporary;
    write_file(temporary.path() / "1x/base/static-base.png", "base");
    write_file(temporary.path() / "1x/arthur/hair.png", "hair");
    write_file(temporary.path() / "1x/rig/hair-mask.png", "mask");
    write_file(temporary.path() / "1x/rig/hair-flow.png", "flow");
    write_file(temporary.path() / "manifest.json", valid_manifest());
    write_file(temporary.path() / "rig.json", valid_rig());

    std::string error;
    const auto manifest = PowerMenuManifest::load(
        temporary.path() / "manifest.json", &error
    );
    assert(manifest.has_value());
    const auto rig = PowerMenuRig::load(
        temporary.path() / "rig.json", *manifest, &error
    );

    assert(rig.has_value());
    assert(error.empty());
    assert(rig->layers.size() == 2U);
    assert(rig->layers[1].animation.type == PowerMenuAnimationType::MeshFlow);
    assert(rig->layers[1].animation.movement_mask_path ==
           temporary.path() / "1x/rig/hair-mask.png");
    assert(rig->layers[1].animation.flow_map_path ==
           temporary.path() / "1x/rig/hair-flow.png");
    assert(rig->layers[1].animation.mesh.strip_count == 20);
    assert(rig->layers[1].animation.flow.pose_count == 5);
}

void loads_complete_authored_project_rig() {
    const std::filesystem::path root(REALMHEART_TEST_POWER_MENU_ROOT);
    std::string error;
    const auto manifest = PowerMenuManifest::load(root / "manifest.json", &error);
    assert(manifest.has_value());
    const auto rig = PowerMenuRig::load(root / "rig.json", *manifest, &error);

    assert(rig.has_value());
    assert(error.empty());
    assert(rig->layers.size() == 17U);
    assert(rig->find_layer("arthur-hair-loose-01") != nullptr);
    assert(rig->find_layer("sylvie-eye-closed-patch") != nullptr);
    assert(rig->find_layer("arthur-rune-glow") != nullptr);
}

void authored_project_rig_is_emphatic_but_safely_bounded() {
    const std::filesystem::path root(REALMHEART_TEST_POWER_MENU_ROOT);
    std::string error;
    const auto manifest = PowerMenuManifest::load(root / "manifest.json", &error);
    assert(manifest.has_value());
    const auto rig = PowerMenuRig::load(root / "rig.json", *manifest, &error);
    assert(rig.has_value());

    for (const std::string_view asset : {
             "arthur-hair-back", "sylvie-hair-back", "sylvie-hair-side"}) {
        const auto* layer = rig->find_layer(asset);
        assert(layer != nullptr);
        assert(layer->animation.mesh.amplitude >= 3.5);
        assert(layer->animation.mesh.amplitude <= 4.5);
        assert(layer->animation.flow.amplitude >= 1.0);
        assert(layer->animation.flow.amplitude <= 1.5);
        assert(layer->animation.flow.frequency >= 0.17);
        assert(layer->animation.flow.frequency <= 0.24);
    }

    for (const std::string_view asset : {
             "arthur-hair-loose-01", "arthur-hair-loose-02",
             "sylvie-hair-loose-01", "sylvie-hair-loose-02"}) {
        const auto* layer = rig->find_layer(asset);
        assert(layer != nullptr);
        assert(layer->animation.rotation_degrees >= 1.0);
        assert(layer->animation.rotation_degrees <= 1.8);
        const double travel = std::hypot(
            layer->animation.translation.x, layer->animation.translation.y
        );
        assert(travel >= 0.7);
        assert(travel <= 1.3);
    }

    for (const std::string_view asset : {"smoke-mana", "smoke-aether"}) {
        const auto* layer = rig->find_layer(asset);
        assert(layer != nullptr);
        const double travel = std::hypot(
            layer->animation.translation.x, layer->animation.translation.y
        );
        assert(travel >= 5.0);
        assert(travel <= 8.0);
        assert(layer->animation.scale_amplitude >= 0.009);
        assert(layer->animation.scale_amplitude <= 0.015);
        assert(layer->animation.opacity_amplitude >= 0.11);
        assert(layer->animation.opacity_amplitude <= 0.18);
    }

    for (const std::string_view asset : {"dust-far", "dust-mid", "dust-front"}) {
        const auto* layer = rig->find_layer(asset);
        assert(layer != nullptr);
        const double travel = std::hypot(
            layer->animation.translation.x, layer->animation.translation.y
        );
        assert(travel >= 4.2);
        assert(travel <= 10.0);
        assert(layer->animation.opacity_amplitude >= 0.12);
        assert(layer->animation.opacity_amplitude <= 0.24);
        assert(layer->animation.frequency >= 0.14);
        assert(layer->animation.frequency <= 0.25);
    }

    const auto* iris = rig->find_layer("sylvie-iris-glow");
    const auto* rune = rig->find_layer("arthur-rune-glow");
    assert(iris != nullptr && rune != nullptr);
    assert(iris->animation.idle_minimum >= 0.34);
    assert(iris->animation.idle_maximum >= 0.88);
    assert(iris->animation.idle_maximum <= 0.96);
    assert(iris->animation.frequency >= 0.20);
    assert(iris->animation.frequency <= 0.28);
    assert(rune->animation.idle_minimum >= 0.05);
    assert(rune->animation.idle_minimum <= 0.10);
    assert(rune->animation.idle_maximum >= 0.98);
    assert(rune->animation.idle_maximum <= 1.0);
    assert(rune->animation.frequency >= 0.45);
    assert(rune->animation.frequency <= 0.60);

    assert(rig->blink.minimum_interval_seconds >= 2.5);
    assert(rig->blink.minimum_interval_seconds <= 3.2);
    assert(rig->blink.maximum_interval_seconds >= 5.5);
    assert(rig->blink.maximum_interval_seconds <= 6.5);
    assert(rig->blink.double_blink_chance >= 0.15);
    assert(rig->blink.double_blink_chance <= 0.22);
    assert(rig->blink.duration_seconds >= 0.69);
    assert(rig->blink.duration_seconds <= 0.71);
}

void require_rig_rejected(std::string rig_text) {
    TemporaryDirectory temporary;
    write_file(temporary.path() / "1x/base/static-base.png", "base");
    write_file(temporary.path() / "1x/arthur/hair.png", "hair");
    write_file(temporary.path() / "1x/rig/hair-mask.png", "mask");
    write_file(temporary.path() / "1x/rig/hair-flow.png", "flow");
    write_file(temporary.path() / "manifest.json", valid_manifest());
    write_file(temporary.path() / "rig.json", rig_text);

    std::string error;
    const auto manifest = PowerMenuManifest::load(
        temporary.path() / "manifest.json", &error
    );
    assert(manifest.has_value());
    const auto rig = PowerMenuRig::load(temporary.path() / "rig.json", *manifest, &error);
    assert(!rig.has_value());
    assert(!error.empty());
}

void rejects_invalid_rig_identity_order_type_maps_and_canvas() {
    const auto replace_once = [](std::string text, std::string_view from,
                                 std::string_view to) {
        const auto position = text.find(from);
        assert(position != std::string::npos);
        text.replace(position, from.size(), to);
        return text;
    };

    require_rig_rejected(replace_once(
        valid_rig(), "\"asset\": \"arthur-hair-back\"",
        "\"asset\": \"static-base\""
    ));
    require_rig_rejected(replace_once(valid_rig(), "\"z\": 1", "\"z\": 0"));
    require_rig_rejected(replace_once(
        valid_rig(), "\"type\": \"mesh-flow\"", "\"type\": \"chaos-goblin\""
    ));
    require_rig_rejected(replace_once(
        valid_rig(), "rig/hair-flow.png", "rig/missing-flow.png"
    ));
    require_rig_rejected(replace_once(valid_rig(), "[1200, 675]", "[1199, 675]"));
}

void loads_valid_manifest_and_preserves_negative_placement() {
    TemporaryDirectory temporary;
    write_file(temporary.path() / "1x/base/static-base.png", "base");
    write_file(temporary.path() / "1x/arthur/hair.png", "hair");
    write_file(temporary.path() / "manifest.json", valid_manifest());

    std::string error;
    const auto manifest = PowerMenuManifest::load(
        temporary.path() / "manifest.json",
        &error
    );

    assert(manifest.has_value());
    assert(error.empty());
    assert(manifest->logical_canvas.width == 1200);
    assert(manifest->logical_canvas.height == 675);

    const auto* hair = manifest->find_asset("arthur-hair-back");
    assert(hair != nullptr);
    assert(hair->placement_1x.x == 312);
    assert(hair->placement_1x.y == -3);
    assert(hair->placement_1x.width == 190);
    assert(hair->placement_1x.height == 252);
}

void rejects_asset_path_escape() {
    TemporaryDirectory temporary;
    const auto outside = temporary.path().parent_path() / "realmheart-outside.png";
    write_file(temporary.path() / "1x/base/static-base.png", "base");
    write_file(outside, "outside");
    write_file(
        temporary.path() / "manifest.json",
        valid_manifest("../realmheart-outside.png")
    );

    std::string error;
    const auto manifest = PowerMenuManifest::load(
        temporary.path() / "manifest.json",
        &error
    );

    assert(!manifest.has_value());
    assert(!error.empty());

    std::error_code remove_error;
    std::filesystem::remove(outside, remove_error);
}

void rejects_duplicate_asset_names() {
    TemporaryDirectory temporary;
    write_file(temporary.path() / "1x/base/static-base.png", "base");
    write_file(temporary.path() / "1x/arthur/hair.png", "hair");

    std::string manifest_text = valid_manifest();
    const std::string original = "\"name\": \"arthur-hair-back\"";
    const auto position = manifest_text.find(original);
    assert(position != std::string::npos);
    manifest_text.replace(
        position,
        original.size(),
        "\"name\": \"static-base\""
    );
    write_file(temporary.path() / "manifest.json", manifest_text);

    std::string error;
    const auto manifest = PowerMenuManifest::load(
        temporary.path() / "manifest.json",
        &error
    );

    assert(!manifest.has_value());
    assert(!error.empty());
}

} // namespace

int main() {
    loads_valid_rig_and_resolves_map_paths();
    loads_complete_authored_project_rig();
    authored_project_rig_is_emphatic_but_safely_bounded();
    rejects_invalid_rig_identity_order_type_maps_and_canvas();
    loads_valid_manifest_and_preserves_negative_placement();
    rejects_asset_path_escape();
    rejects_duplicate_asset_names();
    return 0;
}
