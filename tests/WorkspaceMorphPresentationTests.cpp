#include "ui/workspace/animation/WorkspaceMorphPresentation.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

using realmheart::ui::workspace::animation::kWorkspaceOverviewTransparentCss;
using realmheart::ui::workspace::animation::workspace_morph_draw_opaque_stage;
using realmheart::ui::workspace::animation::workspace_morph_native_reveal_width;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void test_transition_surface_css_is_explicitly_transparent() {
    require(kWorkspaceOverviewTransparentCss.find(
                "window.realmheart-workspace-overview-window") !=
            std::string_view::npos,
            "the toplevel window must have a scoped transparency selector");
    require(kWorkspaceOverviewTransparentCss.find(
                "realmheart-workspace-overview-canvas") !=
            std::string_view::npos,
            "the custom canvas must have a transparency selector");
    require(kWorkspaceOverviewTransparentCss.find(
                "realmheart-workspace-morph-gl-area") !=
            std::string_view::npos,
            "the GL host must have a transparency selector");
    require(kWorkspaceOverviewTransparentCss.find(
                "background-color: transparent") !=
            std::string_view::npos,
            "the CSS must explicitly request a transparent background");
    require(kWorkspaceOverviewTransparentCss.find(
                "background-image: none") !=
            std::string_view::npos,
            "theme background images must be suppressed on the transition surface");
}


void test_shader_ready_frontier_owns_the_final_native_strip() {
    require(workspace_morph_native_reveal_width(640.0, false, false) == 640.0,
            "fallback geometry must retain the complete rectangular reveal");
    require(workspace_morph_native_reveal_width(640.0, true, false) == 588.0,
            "a ready shader must own the final 52 px frontier strip");
    require(workspace_morph_native_reveal_width(640.0, true, true) == 640.0,
            "the exact visible endpoint must hand back the full native scene");
    require(workspace_morph_native_reveal_width(20.0, true, false) == 0.0,
            "the shader inset must never create a negative clip width");
}

void test_opaque_stage_is_owned_only_by_terminal_or_capture_state() {
    require(!workspace_morph_draw_opaque_stage(false, false),
            "a moving morph must leave pixels outside reveal clips transparent");
    require(workspace_morph_draw_opaque_stage(true, false),
            "the stable interactive overview must draw its full stage");
    require(workspace_morph_draw_opaque_stage(false, true),
            "the private native capture must contain the exact terminal scene");
}

} // namespace

int main() {
    try {
        test_transition_surface_css_is_explicitly_transparent();
        test_opaque_stage_is_owned_only_by_terminal_or_capture_state();
        test_shader_ready_frontier_owns_the_final_native_strip();
    } catch (const std::exception& error) {
        std::cerr << "WorkspaceMorphPresentationTests failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "Workspace morph presentation tests passed\n";
    return 0;
}
