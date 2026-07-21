#include "animation/character/CharacterManifest.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace {

using realmheart::animation::character::CharacterLayerPlacement;
using realmheart::animation::character::CharacterLayerRenderer;
using realmheart::animation::character::CharacterManifest;
using realmheart::animation::character::CharacterPlane;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

CharacterManifest load_manifest(int scale) {
    std::string error;
    auto manifest = CharacterManifest::load(
        std::filesystem::path(REALMHEART_TEST_TESSIA_ROOT),
        scale,
        &error
    );
    require(manifest.has_value(), "Tessia manifest must load: " + error);
    return std::move(*manifest);
}

void test_one_x_manifest_uses_exact_export_geometry() {
    const auto manifest = load_manifest(1);
    require(manifest.character == "tessia", "character id must remain Tessia");
    require(manifest.scale_variant == "1x", "standard-density load must choose 1x");
    require(manifest.source_canvas.width == 548 && manifest.source_canvas.height == 1476,
            "1x source canvas must match the published manifest");

    const auto* base = manifest.find_asset("base.png");
    require(base != nullptr, "base asset must exist");
    require(base->size.width == 451 && base->size.height == 1404,
            "1x base dimensions must remain exact");
    require(std::abs(base->offset.x - 48.5) < 0.0001 && base->offset.y == 0.0,
            "half-pixel 1x crop offsets must not be rounded away");
}

void test_two_x_manifest_uses_native_package() {
    const auto manifest = load_manifest(2);
    require(manifest.scale_variant == "2x", "HiDPI load must choose 2x");
    require(manifest.source_canvas.width == 1096 && manifest.source_canvas.height == 2952,
            "2x source canvas must match the native art master");

    const auto* front_hair = manifest.find_asset("hair-front.png");
    require(front_hair != nullptr, "front hair asset must exist");
    require(front_hair->size.width == 676 && front_hair->size.height == 2303,
            "2x front hair dimensions must remain exact");
}

void test_rig_planes_and_host_anchor_are_declarative() {
    const auto manifest = load_manifest(1);
    require(manifest.layers.size() == 7, "static rig must expose seven ordered layers");

    int back_count = 0;
    int front_count = 0;
    for (const auto& layer : manifest.layers) {
        if (layer.plane == CharacterPlane::Back) ++back_count;
        if (layer.plane == CharacterPlane::Front) ++front_count;
    }
    require(back_count == 5 && front_count == 2,
            "rear art, host occlusion, and foreground hands must remain separated");

    const auto* top_hand = manifest.find_layer("top-hand");
    require(top_hand != nullptr, "top hand layer must exist");
    require(top_hand->placement == CharacterLayerPlacement::Host,
            "detached top hand must use a host-relative anchor");
    require(top_hand->host_offset.x == 47.0 && top_hand->host_offset.y == 34.0,
            "top hand tuning must live in the rig manifest rather than C++ drawing code");

    const auto* rear_hair = manifest.find_layer("rear-hair");
    const auto* front_hair = manifest.find_layer("front-hair");
    require(rear_hair != nullptr && front_hair != nullptr,
            "both hair layers must remain declared");
    require(rear_hair->renderer == CharacterLayerRenderer::HairMesh &&
                front_hair->renderer == CharacterLayerRenderer::HairMesh,
            "hair deformation must be selected declaratively");
    require(rear_hair->mask_asset_id == "hair-mask-rear.png" &&
                front_hair->mask_asset_id == "hair-mask-front.png",
            "each mesh layer must name its matching movement mask");
    require(rear_hair->mesh_rows == 24 && front_hair->mesh_rows == 20,
            "mesh density must remain tunable in the rig manifest");
    require(std::abs(rear_hair->idle_strength - 14.0) < 0.0001 &&
                std::abs(front_hair->idle_strength - 10.0) < 0.0001,
            "idle hair amplitudes must remain declarative per layer");
    require(std::abs(rear_hair->idle_phase) < 0.0001 &&
                std::abs(front_hair->idle_phase - 0.82) < 0.0001,
            "rear and front hair must be able to use distinct idle phases");
}

void test_expression_assets_are_declared_and_geometry_safe() {
    const auto manifest = load_manifest(1);
    require(manifest.expression.enabled,
            "Tessia rig must expose its expression controller declaratively");
    require(manifest.expression.eyes_layer_id == "eyes" &&
                manifest.expression.mouth_layer_id == "mouth",
            "expression rig must target the authored eye and mouth layers");
    require(manifest.expression.eyes_inward_asset_id == "eyes-inward.png" &&
                manifest.expression.eyes_user_asset_id == "eyes-user.png" &&
                manifest.expression.eyes_half_asset_id == "eyes-half.png" &&
                manifest.expression.eyes_closed_asset_id == "eyes-closed.png",
            "all eye frames must remain selectable through the rig manifest");
    require(manifest.expression.mouth_curious_asset_id == "mouth-curious.png" &&
                manifest.expression.mouth_smile_asset_id == "mouth-smile.png",
            "both mouth frames must remain selectable through the rig manifest");
}

void test_side_hand_source_anchor_maps_to_host_occlusion_edge() {
    const auto manifest = load_manifest(1);
    const auto* side_hand = manifest.find_asset("hand-side.png");
    require(side_hand != nullptr, "side hand must exist");

    const double source_anchor_x = manifest.placement.source_anchor.x *
        static_cast<double>(manifest.source_canvas.width);
    require(std::abs(source_anchor_x - side_hand->offset.x) < 0.0001,
            "shared character transform must anchor the side hand crop to the host edge");
}

} // namespace

int main() {
    test_one_x_manifest_uses_exact_export_geometry();
    test_two_x_manifest_uses_native_package();
    test_rig_planes_and_host_anchor_are_declarative();
    test_expression_assets_are_declared_and_geometry_safe();
    test_side_hand_source_anchor_maps_to_host_occlusion_edge();
    std::cout << "Character manifest tests passed\n";
    return 0;
}
