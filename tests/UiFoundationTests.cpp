#include "ui/AssetResolver.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/ImageFileFilters.hpp"
#include "ui/styles/CssModuleLoader.hpp"

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
    const auto icon = realmheart::ui::resolve_icon(root, "wifi.svg");
    require(icon.has_value(), "known project icon must resolve");
    require(icon->filename() == "wifi.svg", "resolved icon must retain its logical filename");
    require(!realmheart::ui::resolve_icon(root, "missing.svg"), "missing icon must not resolve");
    require(!realmheart::ui::resolve_icon(root, "../wifi.svg"), "parent traversal must be rejected");
    require(!realmheart::ui::resolve_icon(root, "/tmp/wifi.svg"), "absolute icon paths must be rejected");
    require(!realmheart::ui::resolve_icon(root, "nested/wifi.svg"), "logical icon names must not smuggle directories");
}

void test_project_asset_resolver_allows_safe_nested_realmheart_assets() {
    const auto icon = realmheart::ui::resolve_project_asset(
        "Realmheart-Icons/battery-charging-50.svg"
    );
    require(icon.has_value(), "nested Realmheart icon paths must resolve from project assets");
    require(icon->filename() == "battery-charging-50.svg",
            "nested resolver must retain the requested asset filename");
    require(!realmheart::ui::resolve_project_asset("../CMakeLists.txt"),
            "nested resolver must reject direct parent traversal");
    require(!realmheart::ui::resolve_project_asset(
        "Realmheart-Icons/../../CMakeLists.txt"
    ), "nested resolver must reject traversal after a valid prefix");
    require(!realmheart::ui::resolve_project_asset("/tmp/battery-charging-50.svg"),
            "nested resolver must reject absolute paths");
}

void test_style_resolver_uses_dedicated_roots_and_rejects_escape_paths() {
    const auto taskbar_css = realmheart::ui::styles::resolve_style_module(
        "taskbar/bar.css"
    );
    require(taskbar_css.has_value(), "modular taskbar CSS must resolve from style roots");
    require(taskbar_css->filename() == "bar.css",
            "resolved taskbar style must retain its filename");
    require(!realmheart::ui::styles::resolve_style_module("../CMakeLists.txt"),
            "style resolver must reject direct parent traversal");
    require(!realmheart::ui::styles::resolve_style_module(
        "taskbar/../../CMakeLists.txt"
    ), "style resolver must reject traversal after a valid prefix");
    require(!realmheart::ui::styles::resolve_style_module("/tmp/bar.css"),
            "style resolver must reject absolute paths");
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

void test_wallpaper_surface_spec_is_fullscreen_background_and_noninteractive() {
    const auto spec = realmheart::ui::make_wallpaper_surface_spec();
    require(spec.surface_namespace == "realmheart-wallpaper", "wallpaper namespace must remain stable");
    require(spec.layer == realmheart::ui::LayerSurfaceLevel::Background,
            "wallpaper must render on the background layer");
    require(spec.keyboard_mode == realmheart::ui::LayerKeyboardMode::None,
            "wallpaper must never request keyboard input");
    require(spec.anchor_left && spec.anchor_right && spec.anchor_top && spec.anchor_bottom,
            "wallpaper must fill the complete monitor");
    require(spec.exclusive_zone == 0, "wallpaper must not reserve compositor space");
    require(spec.margin_left == 0 && spec.margin_right == 0 &&
            spec.margin_top == 0 && spec.margin_bottom == 0,
            "wallpaper must not leave monitor-edge gaps");
}

void test_test_surface_spec_is_nonexclusive() {
    const auto spec = realmheart::ui::make_test_surface_spec();
    require(spec.surface_namespace == "realmheart-test-layer", "test namespace must remain stable");
    require(spec.anchor_left && spec.anchor_top, "test surface must anchor top-left");
    require(!spec.anchor_bottom && !spec.anchor_right, "test surface must not stretch across an edge");
    require(spec.exclusive_zone == 0, "test surface must not reserve compositor space");
}

void mark_destroyed(gpointer data, GObject* /*object*/) {
    *static_cast<bool*>(data) = true;
}

void test_image_file_filter_model_owns_exactly_one_valid_filter() {
    GListModel* filters = realmheart::ui::create_image_file_filters();
    require(filters != nullptr, "image filter model must be created");
    require(g_list_model_get_n_items(filters) == 1, "image filter model must contain one filter");

    GObject* filter = G_OBJECT(g_list_model_get_item(filters, 0));
    require(filter != nullptr && GTK_IS_FILE_FILTER(filter),
            "image filter model item must be a GtkFileFilter");

    bool destroyed = false;
    g_object_weak_ref(filter, mark_destroyed, &destroyed);
    g_object_unref(filter);
    require(!destroyed, "filter must remain alive while its model owns it");
    g_object_unref(filters);
    require(destroyed, "filter must be released when its model is released");
}

} // namespace

int main() {
    test_asset_resolver_accepts_known_icons_and_rejects_escape_paths();
    test_project_asset_resolver_allows_safe_nested_realmheart_assets();
    test_style_resolver_uses_dedicated_roots_and_rejects_escape_paths();
    test_bar_surface_spec_is_reusable_and_reserves_its_width();
    test_wallpaper_surface_spec_is_fullscreen_background_and_noninteractive();
    test_test_surface_spec_is_nonexclusive();
    test_image_file_filter_model_owns_exactly_one_valid_filter();
    std::cout << "UI foundation tests passed\n";
    return 0;
}
