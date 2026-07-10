#include "ui/AssetResolver.hpp"
#include "ui/LayerSurface.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_asset_resolver_accepts_known_icons_and_rejects_escape_paths() {
    const auto root = std::filesystem::path(REALMHEART_TEST_ASSET_DIR);
    const auto icon = realmheart::ui::resolve_icon(root, "wifi-4.svg");
    require(icon.has_value(), "known project icon must resolve");
    require(icon->filename() == "wifi-4.svg", "resolved icon must retain its logical filename");
    require(!realmheart::ui::resolve_icon(root, "missing.svg"), "missing icon must not resolve");
    require(!realmheart::ui::resolve_icon(root, "../wifi-4.svg"), "parent traversal must be rejected");
    require(!realmheart::ui::resolve_icon(root, "/tmp/wifi-4.svg"), "absolute icon paths must be rejected");
    require(!realmheart::ui::resolve_icon(root, "nested/wifi-4.svg"), "logical icon names must not smuggle directories");
}

void test_bar_surface_spec_is_reusable_and_reserves_its_width() {
    const auto spec = realmheart::ui::make_bar_surface_spec(72);
    require(spec.surface_namespace == "realmheart-bar", "bar namespace must remain stable");
    require(spec.anchor_left && spec.anchor_top && spec.anchor_bottom && !spec.anchor_right,
            "vertical bar must anchor to the complete left edge");
    require(spec.exclusive_zone == 72, "bar must reserve exactly its configured width");
    require(spec.layer == realmheart::ui::LayerSurfaceLevel::Top, "bar must use the top layer");
    require(spec.keyboard_mode == realmheart::ui::LayerKeyboardMode::OnDemand,
            "bar keyboard access must remain on-demand");
}

void test_test_surface_spec_is_nonexclusive() {
    const auto spec = realmheart::ui::make_test_surface_spec();
    require(spec.surface_namespace == "realmheart-test-layer", "test namespace must remain stable");
    require(spec.anchor_left && spec.anchor_top, "test surface must anchor top-left");
    require(!spec.anchor_bottom && !spec.anchor_right, "test surface must not stretch across an edge");
    require(spec.exclusive_zone == 0, "test surface must not reserve compositor space");
}

} // namespace

int main() {
    test_asset_resolver_accepts_known_icons_and_rejects_escape_paths();
    test_bar_surface_spec_is_reusable_and_reserves_its_width();
    test_test_surface_spec_is_nonexclusive();
    std::cout << "UI foundation tests passed\n";
    return 0;
}
