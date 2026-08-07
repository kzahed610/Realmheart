#include "ui/workspace/animation/WorkspaceMorphModel.hpp"
#include "ui/workspace/animation/WorkspaceMorphShaderData.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using namespace realmheart::ui::workspace::animation;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(float actual, float expected, const char* message) {
    if (std::abs(actual - expected) > 0.0001F) {
        throw std::runtime_error(message);
    }
}

WorkspaceMorphLayout test_layout() {
    const std::array<int, 4> ids{{6, 7, 8, 9}};
    const std::array<double, 4> heights{{120.0, 240.0, 360.0, 360.0}};
    const std::vector<WorkspaceMorphSource> sources{
        {6, {12.0, 100.0, 24.0, 30.0}, false, true},
        {7, {12.0, 200.0, 24.0, 30.0}, false, true},
        {8, {12.0, 300.0, 24.0, 30.0}, true, true},
        {9, {12.0, 400.0, 24.0, 30.0}, false, true},
    };
    return build_workspace_morph_layout(
        ids,
        heights,
        sources,
        1920.0,
        1080.0
    );
}

void test_geometry_is_normalized_and_keeps_workspace_styles() {
    const auto layout = test_layout();
    const auto geometry = build_workspace_morph_shader_geometry(layout);

    require_near(geometry.band_top[0], 0.0F,
                 "first shader band must start at the top");
    require_near(geometry.band_bottom[0], 120.0F / 1080.0F,
                 "first shader band must preserve its exact bottom");
    require_near(geometry.band_top[2], 360.0F / 1080.0F,
                 "third shader band must preserve cumulative geometry");
    require_near(geometry.band_bottom[3], 1.0F,
                 "last shader band must end at the viewport bottom");

    require_near(geometry.element_style[0], 1.0F,
                 "workspace six must use the Water shader style");
    require_near(geometry.element_style[1], 2.0F,
                 "workspace seven must use the Wind shader style");
    require_near(geometry.element_style[2], 3.0F,
                 "workspace eight must use the Earth shader style");
    require_near(geometry.element_style[3], 0.0F,
                 "workspace nine must cycle back to Fire");

    require_near(geometry.source_y[2], 315.0F / 1080.0F,
                 "shader source Y must use the captured rune center");
    require(geometry.origin[0] > 0.0F && geometry.origin[0] < 0.1F,
            "shader origin must remain anchored to the left taskbar");
}

void test_fronts_follow_the_geometry_reveal_exactly() {
    const auto layout = test_layout();
    const auto hidden = sample_workspace_morph_frame(layout, 0.0);
    const auto middle = sample_workspace_morph_frame(layout, 0.55);
    const auto visible = sample_workspace_morph_frame(layout, 1.0);

    const auto hidden_shader = build_workspace_morph_shader_frame(
        layout,
        hidden
    );
    const auto middle_shader = build_workspace_morph_shader_frame(
        layout,
        middle
    );
    const auto visible_shader = build_workspace_morph_shader_frame(
        layout,
        visible
    );

    require_near(hidden_shader.reveal_left_x[0], 12.0F / 1920.0F,
                 "hidden shader reveal must start at the rune left edge");
    require_near(hidden_shader.front_x[0], 36.0F / 1920.0F,
                 "hidden shader frontier must end at the rune right edge");
    require_near(hidden_shader.front_top[0], 100.0F / 1080.0F,
                 "hidden shader frontier must start at the rune top");
    require_near(hidden_shader.front_bottom[0], 130.0F / 1080.0F,
                 "hidden shader frontier must end at the rune bottom");

    const auto ignition = sample_workspace_morph_frame(layout, 0.06);
    const auto ignition_shader = build_workspace_morph_shader_frame(
        layout,
        ignition
    );
    require_near(ignition_shader.front_top[0], 100.0F / 1080.0F,
                 "shader ignition must remain vertically local to the rune");
    require(ignition_shader.front_x[0] > hidden_shader.front_x[0],
            "shader ignition must breathe outward from the rune edge");
    require(middle_shader.front_x[0] > ignition_shader.front_x[0],
            "shader frontier must advance after the local ignition");
    require_near(visible_shader.front_x[0], 1.0F,
                 "visible shader frontier must reach the exact right edge");
    require_near(visible_shader.front_x[3], 1.0F,
                 "all shader bands must finish at the exact endpoint");
}

void test_invalid_extents_stay_finite() {
    WorkspaceMorphLayout layout;
    layout.width = 0.0;
    layout.height = 0.0;
    const auto geometry = build_workspace_morph_shader_geometry(layout);
    const auto frame = build_workspace_morph_shader_frame(layout, {});

    require(std::isfinite(geometry.origin[0]),
            "invalid width must not produce a non-finite origin");
    require(std::isfinite(geometry.band_bottom[3]),
            "invalid height must not produce non-finite band bounds");
    require(std::isfinite(frame.reveal_left_x[0]),
            "invalid width must not produce a non-finite reveal root");
    require(std::isfinite(frame.front_x[0]),
            "invalid width must not produce a non-finite frontier");
    require(std::isfinite(frame.front_top[0]) &&
            std::isfinite(frame.front_bottom[0]),
            "invalid height must not produce non-finite frontier bounds");
}

} // namespace

int main() {
    try {
        test_geometry_is_normalized_and_keeps_workspace_styles();
        test_fronts_follow_the_geometry_reveal_exactly();
        test_invalid_extents_stay_finite();
    } catch (const std::exception& error) {
        std::cerr << "WorkspaceMorphShaderDataTests failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "Workspace morph shader data tests passed\n";
    return 0;
}
