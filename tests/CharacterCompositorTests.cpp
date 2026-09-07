#include "animation/character/CharacterCompositor.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

using realmheart::animation::character::CharacterCompositor;
using realmheart::animation::character::CharacterHairMode;
using realmheart::animation::character::CharacterHostGeometry;
using realmheart::core::DisplayTier;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::unique_ptr<CharacterCompositor> create_compositor(
    GtkWidget* back_host,
    GtkWidget* front_host,
    CharacterHairMode mode,
    std::string* error
) {
    return CharacterCompositor::create(
        back_host,
        front_host,
        std::filesystem::path(REALMHEART_TEST_TESSIA_ROOT),
        DisplayTier::P1080,
        DisplayTier::P1080,
        {
            .occlusion_left = 0.0,
            .occlusion_top = 0.0,
            .surface_width = 726,
            .surface_height = 1476,
        },
        error,
        mode
    );
}

void test_static_mode_avoids_mesh_and_flow_resources() {
    std::string error;
    GtkWidget* back_host = GTK_WIDGET(g_object_ref_sink(gtk_fixed_new()));
    GtkWidget* front_host = GTK_WIDGET(g_object_ref_sink(gtk_fixed_new()));
    auto compositor = create_compositor(
        back_host, front_host, CharacterHairMode::Static, &error
    );
    require(compositor != nullptr, "static compositor must construct: " + error);
    require(!compositor->mesh_resources_loaded(),
            "static construction must not build mesh resources");
    require(!compositor->flow_caches_loaded(),
            "static construction must not build flow resources");
    require(compositor->hair_mesh_cache_count() == 2U,
            "static construction may retain only one source texture per hair layer");

    require(compositor->set_hair_mode(CharacterHairMode::Mesh, &error),
            "static-to-mesh transition must succeed: " + error);
    require(compositor->mesh_resources_loaded(),
            "mesh transition must build mesh resources");
    require(!compositor->flow_caches_loaded(),
            "mesh mode must not retain flow resources");

    require(compositor->set_hair_mode(CharacterHairMode::MeshFlow, &error),
            "mesh-to-flow transition must succeed: " + error);
    if (compositor->flow_caches_loaded()) {
        require(compositor->mesh_resources_loaded(),
                "flow mode must retain mesh resources");
    } else {
        require(!compositor->mesh_resources_loaded(),
                "failed flow activation must release mesh resources after static fallback");
        require(!error.empty(),
                "static fallback after flow failure must preserve a diagnostic");
    }

    require(compositor->set_hair_mode(CharacterHairMode::Static, &error),
            "flow-to-static transition must release mode resources: " + error);
    require(!compositor->mesh_resources_loaded(),
            "static transition must release mesh resources");
    require(!compositor->flow_caches_loaded(),
            "static transition must release flow resources");

    compositor.reset();
    g_object_unref(back_host);
    g_object_unref(front_host);
}

} // namespace

int main() {
    if (!gtk_init_check()) {
        std::cout << "Character compositor tests skipped: GTK display unavailable\n";
        return 0;
    }
    test_static_mode_avoids_mesh_and_flow_resources();
    std::cout << "Character compositor tests passed\n";
    return 0;
}
