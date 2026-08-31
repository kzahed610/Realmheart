#include "animation/character/CharacterManifest.hpp"
#include "core/DisplayTier.hpp"
#include "ui/sidebar/SidebarGeometry.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

namespace {

using realmheart::animation::character::CharacterLayerPlacement;
using realmheart::animation::character::CharacterLayerRenderer;
using realmheart::animation::character::CharacterManifest;
using realmheart::animation::character::CharacterPlane;
using realmheart::animation::character::host_layer_offset_for_display_tier;
using realmheart::core::DisplayTier;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

CharacterManifest load_manifest(DisplayTier tier) {
    std::string error;
    auto manifest = CharacterManifest::load(
        std::filesystem::path(REALMHEART_TEST_TESSIA_ROOT),
        tier,
        &error
    );
    require(manifest.has_value(), "Tessia manifest must load: " + error);
    return std::move(*manifest);
}

void test_1080p_manifest_uses_exact_export_geometry() {
    const auto manifest = load_manifest(DisplayTier::P1080);
    require(manifest.character == "tessia", "character id must remain Tessia");
    require(manifest.display_tier == DisplayTier::P1080, "standard-resolution load must choose 1080p");
    require(manifest.source_canvas.width == 548 && manifest.source_canvas.height == 1476,
            "1080p source canvas must match the published manifest");

    const auto* base = manifest.find_asset("base.png");
    require(base != nullptr, "base asset must exist");
    require(base->size.width == 451 && base->size.height == 1404,
            "1080p base dimensions must remain exact");
    require(std::abs(base->offset.x - 48.5) < 0.0001 && base->offset.y == 0.0,
            "half-pixel 1080p crop offsets must not be rounded away");
}

void test_1440p_manifest_uses_direct_1080p_derivative_geometry() {
    const auto manifest = load_manifest(DisplayTier::P1440);
    require(manifest.display_tier == DisplayTier::P1440, "1440p load must choose the 1440p package");
    require(manifest.source_canvas.width == 731 && manifest.source_canvas.height == 1968,
            "1440p source canvas must use direct four-thirds geometry");

    const auto* front_hair = manifest.find_asset("hair-front.png");
    require(front_hair != nullptr, "front hair asset must exist");
    require(front_hair->size.width == 451 && front_hair->size.height == 1536,
            "1440p front hair dimensions must use direct four-thirds geometry");
    require(std::abs(front_hair->offset.x - 196.0) < 0.0001,
            "1440p front hair offset must use direct four-thirds geometry");
}

void test_4k_manifest_uses_direct_1080p_derivative_geometry() {
    const auto manifest = load_manifest(DisplayTier::P4K);
    require(manifest.display_tier == DisplayTier::P4K, "4K load must choose the 4K package");
    require(manifest.source_canvas.width == 1096 && manifest.source_canvas.height == 2952,
            "4K source canvas must double the canonical 1080p geometry");

    const auto* front_hair = manifest.find_asset("hair-front.png");
    require(front_hair != nullptr, "front hair asset must exist");
    require(front_hair->size.width == 676 && front_hair->size.height == 2304,
            "4K front hair dimensions must double the canonical 1080p geometry");
    require(front_hair->offset.x == 294.0,
            "4K front hair offset must double the canonical 1080p geometry");
}

void test_all_tier_packages_contain_complete_character_families() {
    const std::array tiers{
        DisplayTier::P1080,
        DisplayTier::P1440,
        DisplayTier::P4K,
    };
    const std::array asset_ids{
        "base.png",
        "hand-top.png",
        "hand-side.png",
        "hair-rear.png",
        "hair-flow-rear.png",
        "hair-mask-rear.png",
        "hair-front.png",
        "hair-flow-front.png",
        "hair-mask-front.png",
        "eyes-user.png",
        "eyes-inward.png",
        "eyes-half.png",
        "eyes-closed.png",
        "mouth-smile.png",
        "mouth-curious.png",
    };

    for (const auto tier : tiers) {
        const auto manifest = load_manifest(tier);
        require(manifest.assets.size() == asset_ids.size(),
                "every display tier must expose all Tessia assets");
        const auto expected_directory = std::string(
            realmheart::core::display_tier_directory(tier)
        );
        for (const auto asset_id : asset_ids) {
            const auto* asset = manifest.find_asset(asset_id);
            require(asset != nullptr, "every display tier must contain " + std::string(asset_id));
            require(asset->path.parent_path().filename() == expected_directory,
                    "selected asset must come from the requested display-tier directory");
        }
    }
}

void test_missing_higher_tier_falls_back_to_canonical_1080p() {
    namespace fs = std::filesystem;
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const auto temporary_root = fs::temp_directory_path() /
        ("realmheart-character-manifest-fallback-" + suffix);
    std::error_code error;
    fs::create_directories(temporary_root, error);
    require(!error, "fallback fixture root must be creatable");
    fs::copy_file(
        fs::path(REALMHEART_TEST_TESSIA_ROOT) / "rig.json",
        temporary_root / "rig.json",
        fs::copy_options::overwrite_existing,
        error
    );
    require(!error, "fallback fixture must contain rig.json");
    fs::copy(
        fs::path(REALMHEART_TEST_TESSIA_ROOT) / "1080p",
        temporary_root / "1080p",
        fs::copy_options::recursive,
        error
    );
    require(!error, "fallback fixture must contain the canonical package");

    std::string load_error;
    const auto manifest = CharacterManifest::load(
        temporary_root,
        DisplayTier::P1440,
        &load_error
    );
    require(manifest.has_value(), "missing higher tier must still load: " + load_error);
    require(manifest->display_tier == DisplayTier::P1080,
            "missing higher tier must select canonical 1080p");
    require(manifest->find_asset("base.png") != nullptr,
            "fallback package must remain fully loadable");

    fs::remove_all(temporary_root, error);
    require(!error, "fallback fixture cleanup must succeed");
}

void test_rig_planes_and_host_anchor_are_declarative() {
    const auto manifest = load_manifest(DisplayTier::P1080);
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
    require(rear_hair->flow_asset_id == "hair-flow-rear.png" &&
                front_hair->flow_asset_id.empty(),
            "rear-only directional flow must remain declarative");
    require(std::abs(rear_hair->flow_strength - 1.6) < 0.0001 &&
                std::abs(rear_hair->flow_frequency - 0.92) < 0.0001,
            "rear flow amplitude and timing must remain rig-controlled");
}

void test_host_anchored_layers_follow_layout_tier_without_mutating_rig() {
    const auto manifest = load_manifest(DisplayTier::P1080);
    const auto* top_hand = manifest.find_layer("top-hand");
    require(top_hand != nullptr, "top hand layer must exist");

    const auto p1080 = host_layer_offset_for_display_tier(
        top_hand->host_offset, DisplayTier::P1080
    );
    const auto p1440 = host_layer_offset_for_display_tier(
        top_hand->host_offset, DisplayTier::P1440
    );
    const auto p4k = host_layer_offset_for_display_tier(
        top_hand->host_offset, DisplayTier::P4K
    );

    require(std::abs(p1080.x - 47.0) < 0.0001 &&
                std::abs(p1080.y - 34.0) < 0.0001,
            "1080p top-hand anchor must remain byte-for-byte visually canonical");
    require(std::abs(p1440.x - (47.0 * 4.0 / 3.0)) < 0.0001 &&
                std::abs(p1440.y - (34.0 * 4.0 / 3.0)) < 0.0001,
            "1440p top-hand anchor must follow the four-thirds layout transform");
    require(std::abs(p4k.x - 94.0) < 0.0001 &&
                std::abs(p4k.y - 68.0) < 0.0001,
            "4K top-hand anchor must follow the two-times layout transform");

    require(top_hand->host_offset.x == 47.0 && top_hand->host_offset.y == 34.0,
            "display-tier placement must not rewrite canonical rig tuning");
}

void test_expression_assets_are_declared_and_geometry_safe() {
    const auto manifest = load_manifest(DisplayTier::P1080);
    require(manifest.expression.enabled,
            "Tessia rig must expose its expression controller declaratively");
    require(manifest.expression.eyes_layer_id == "eyes" &&
                manifest.expression.mouth_layer_id == "mouth",
            "expression rig must target the authored eye and mouth layers");
    require(manifest.expression.eyes_inward_asset_id == "eyes-inward.png" &&
                manifest.expression.eyes_half_asset_id == "eyes-half.png" &&
                manifest.expression.eyes_closed_asset_id == "eyes-closed.png",
            "only temporary eye overlays must be selected through the rig manifest");
    require(manifest.expression.mouth_curious_asset_id == "mouth-curious.png",
            "only the temporary curious-mouth overlay must be selected through the rig manifest");
}

void test_side_hand_source_anchor_maps_to_host_occlusion_edge() {
    const auto manifest = load_manifest(DisplayTier::P1080);
    const auto* side_hand = manifest.find_asset("hand-side.png");
    require(side_hand != nullptr, "side hand must exist");

    const double source_anchor_x = manifest.placement.source_anchor.x *
        static_cast<double>(manifest.source_canvas.width);
    require(std::abs(source_anchor_x - side_hand->offset.x) < 0.0001,
            "shared character transform must anchor the side hand crop to the host edge");
}

void test_tiered_tessia_bounds_fit_the_sidebar_host() {
    using realmheart::core::display_tier_name;
    using realmheart::core::display_tier_spec;
    using realmheart::ui::sidebar::SidebarFrameLayout;
    using realmheart::ui::sidebar::sidebar_height_for_logical_height;

    for (const auto tier : {
        DisplayTier::P1080,
        DisplayTier::P1440,
        DisplayTier::P4K,
    }) {
        const auto manifest = load_manifest(tier);
        const auto sidebar = SidebarFrameLayout::for_display_tier(tier);
        const auto tier_spec = display_tier_spec(tier);
        const int host_height = sidebar_height_for_logical_height(
            tier_spec.logical_height
        );
        const double scale =
            static_cast<double>(host_height) * manifest.placement.height_fraction /
            static_cast<double>(manifest.source_canvas.height);
        const double source_anchor_x = manifest.placement.source_anchor.x *
            static_cast<double>(manifest.source_canvas.width);
        const double source_anchor_y = manifest.placement.source_anchor.y *
            static_cast<double>(manifest.source_canvas.height);
        const double source_origin_x = sidebar.frame_origin_x() +
            manifest.placement.host_offset.x - source_anchor_x * scale;
        const double source_origin_y = manifest.placement.host_offset.y -
            source_anchor_y * scale;

        double left = std::numeric_limits<double>::infinity();
        double top = std::numeric_limits<double>::infinity();
        double right = -std::numeric_limits<double>::infinity();
        double bottom = -std::numeric_limits<double>::infinity();
        for (const auto& layer : manifest.layers) {
            if (!layer.visible) continue;
            const auto* asset = manifest.find_asset(layer.asset_id);
            require(asset != nullptr, "visible Tessia layer must have an asset");

            const bool source_anchored =
                layer.placement == CharacterLayerPlacement::SourceCanvas;
            const auto host_offset = host_layer_offset_for_display_tier(
                layer.host_offset, tier
            );
            const double x = source_anchored
                ? source_origin_x + asset->offset.x * scale
                : static_cast<double>(sidebar.frame_origin_x()) + host_offset.x;
            const double y = source_anchored
                ? source_origin_y + asset->offset.y * scale
                : host_offset.y;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x + asset->size.width * scale);
            bottom = std::max(bottom, y + asset->size.height * scale);
        }

        const std::string tier_name(display_tier_name(tier));
        require(left >= 0.0,
                tier_name + " Tessia crop must not escape the sidebar's left host edge");
        require(top >= 0.0,
                tier_name + " Tessia crop must not escape the sidebar's top host edge");
        require(right <= static_cast<double>(sidebar.surface_width()),
                tier_name + " Tessia crop must not escape the sidebar's right host edge");
        require(bottom <= static_cast<double>(host_height),
                tier_name + " Tessia crop must not escape the sidebar's bottom host edge");
    }
}

} // namespace

int main() {
    test_1080p_manifest_uses_exact_export_geometry();
    test_1440p_manifest_uses_direct_1080p_derivative_geometry();
    test_4k_manifest_uses_direct_1080p_derivative_geometry();
    test_all_tier_packages_contain_complete_character_families();
    test_missing_higher_tier_falls_back_to_canonical_1080p();
    test_rig_planes_and_host_anchor_are_declarative();
    test_host_anchored_layers_follow_layout_tier_without_mutating_rig();
    test_expression_assets_are_declared_and_geometry_safe();
    test_side_hand_source_anchor_maps_to_host_occlusion_edge();
    test_tiered_tessia_bounds_fit_the_sidebar_host();
    std::cout << "Character manifest tests passed\n";
    return 0;
}
