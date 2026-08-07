#include "ui/workspace/animation/WorkspaceMorphModel.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using realmheart::ui::workspace::animation::WorkspaceMorphRect;
using realmheart::ui::workspace::animation::WorkspaceMorphSource;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) > 0.0001) {
        throw std::runtime_error(message);
    }
}

void test_maps_sources_by_actual_workspace_id() {
    const std::array<int, 4> ids{{6, 7, 8, 9}};
    const std::array<double, 4> heights{{118.8, 118.8, 604.8, 237.6}};
    const std::vector<WorkspaceMorphSource> sources{
        {9, {10.0, 400.0, 25.0, 31.0}, false, true},
        {6, {10.0, 280.0, 25.0, 31.0}, false, false},
        {8, {10.0, 360.0, 25.0, 31.0}, true, true},
        {7, {10.0, 320.0, 25.0, 31.0}, false, true},
    };

    const auto layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, sources, 1920.0, 1080.0
        );

    require_near(layout.bands[0].source.y, 280.0,
                 "workspace six must use its own rune source");
    require_near(layout.bands[3].source.y, 400.0,
                 "workspace nine must use its own rune source");
    require(layout.bands[0].style_index == 1,
            "workspace six must cycle to Water");
    require(layout.bands[3].style_index == 0,
            "workspace nine must cycle to Fire");
    require(layout.bands[2].active && layout.bands[2].occupied,
            "the exact rune visual state must follow its workspace ID");
}



void test_missing_workspace_source_never_borrows_another_id() {
    const std::array<int, 4> ids{{6, 7, 8, 9}};
    const std::array<double, 4> heights{{270.0, 270.0, 270.0, 270.0}};
    const std::vector<WorkspaceMorphSource> sources{
        {9, {90.0, 900.0, 25.0, 31.0}, false, true},
    };

    const auto layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, sources, 1920.0, 1080.0
        );
    require_near(layout.bands[3].source.y, 900.0,
                 "captured workspace nine must use its exact source");
    require(layout.bands[0].source.y != 900.0,
            "missing workspace six must not borrow workspace nine geometry");
}

void test_source_geometry_scaling_is_applied_once() {
    const std::vector<WorkspaceMorphSource> sources{
        {6, {15.5, 240.0, 25.0, 31.0}, true, true},
    };
    const auto scaled =
        realmheart::ui::workspace::animation::
            scale_workspace_morph_sources_to_reference(
                sources, 1280.0 / 1920.0, 720.0 / 1080.0
            );

    require(scaled.size() == 1, "source scaling must preserve source count");
    require_near(scaled[0].bounds.x, 23.25,
                 "bar x offset must be scaled exactly once");
    require_near(scaled[0].bounds.y, 360.0,
                 "logical y must map into reference space");
    require_near(scaled[0].bounds.width, 37.5,
                 "source width must map into reference space");
    require(scaled[0].active && scaled[0].occupied,
            "visual state must survive geometry scaling");

    const std::array<int, 4> ids{{6, 7, 8, 9}};
    const std::array<double, 4> heights{{270.0, 270.0, 270.0, 270.0}};
    const auto layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, scaled, 1920.0, 1080.0
        );
    require_near(layout.bands[0].source.x, 23.25,
                 "layout construction must not apply the bar offset twice");
}

void test_active_and_occupied_state_do_not_change_bounds() {
    const std::array<int, 4> ids{{1, 2, 3, 4}};
    const std::array<double, 4> heights{{270.0, 270.0, 270.0, 270.0}};
    const std::vector<WorkspaceMorphSource> inactive{
        {1, {12.0, 200.0, 25.0, 31.0}, false, false},
    };
    const std::vector<WorkspaceMorphSource> active{
        {1, {12.0, 200.0, 25.0, 31.0}, true, true},
    };

    const auto inactive_layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, inactive, 1920.0, 1080.0
        );
    const auto active_layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, active, 1920.0, 1080.0
        );

    require_near(inactive_layout.bands[0].source.x,
                 active_layout.bands[0].source.x,
                 "active visual state must not move the captured rune bounds");
    require_near(inactive_layout.bands[0].source.height,
                 active_layout.bands[0].source.height,
                 "occupied visual state must not resize captured rune bounds");
}

void test_destination_bands_preserve_exact_heights() {
    const std::array<int, 4> ids{{1, 2, 3, 4}};
    const std::array<double, 4> heights{{118.8, 604.8, 118.8, 237.6}};
    const auto layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, {}, 1920.0, 1080.0
        );

    require_near(layout.bands[0].destination.y, 0.0,
                 "first band must start at zero");
    require_near(layout.bands[1].destination.y, 118.8,
                 "second band must follow the first height");
    require_near(layout.bands[2].destination.y, 723.6,
                 "third band must follow the active band");
    require_near(layout.bands[3].destination.height, 237.6,
                 "last band must consume the exact remaining height");
}

void test_hidden_and_visible_endpoints_are_exact() {
    const std::array<int, 4> ids{{1, 2, 3, 4}};
    const std::array<double, 4> heights{{118.8, 604.8, 118.8, 237.6}};
    const auto layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, {}, 1920.0, 1080.0
        );

    const auto hidden =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.0
        );
    require(hidden.exact_hidden, "zero progress must be the hidden endpoint");
    require(!hidden.exact_visible,
            "zero progress must not be the visible endpoint");
    require_near(hidden.bands[0].realm_opacity, 0.0,
                 "hidden endpoint must not reveal realm content");
    require_near(
        hidden.bands[0].reveal_clip.x,
        layout.bands[0].source.x,
        "hidden endpoint must stay rooted at the workspace rune"
    );
    require_near(
        hidden.bands[0].reveal_clip.width,
        layout.bands[0].source.width,
        "hidden endpoint geometry must collapse into the workspace rune"
    );
    require_near(
        hidden.bands[0].reveal_clip.height,
        layout.bands[0].source.height,
        "hidden endpoint must retain only rune-local vertical extent"
    );

    const auto visible =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 1.0
        );
    require(visible.exact_visible,
            "one progress must be the visible endpoint");
    require_near(visible.bands[2].reveal_clip.width, 1920.0,
                 "visible endpoint must reveal the full width");
    require_near(
        visible.bands[2].reveal_clip.height,
        layout.bands[2].destination.height,
        "visible endpoint must match destination geometry exactly"
    );
    require_near(visible.bands[2].card_opacity, 1.0,
                 "visible endpoint must fully reveal cards");
    require_near(visible.bands[2].proxy_opacity, 0.0,
                 "visible endpoint must release transition proxies");
}

void test_stages_overlap_without_early_cards() {
    const std::array<int, 4> ids{{1, 2, 3, 4}};
    const std::array<double, 4> heights{{118.8, 604.8, 118.8, 237.6}};
    const std::vector<WorkspaceMorphSource> sources{
        {1, {25.0, 220.0, 25.0, 31.0}, true, true},
    };
    const auto layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, sources, 1920.0, 1080.0
        );

    const auto structural =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.34
        );
    require_near(structural.bands[0].proxy_opacity, 0.0,
                 "structural phase must not resurrect a proxy rail");
    require(structural.bands[0].reveal_clip.width > 50.0,
            "realm propagation must advance from the captured rune edge");
    require_near(structural.bands[0].card_opacity, 0.0,
                 "cards must not appear during structural unfolding");
    require_near(structural.separator_opacity, 0.0,
                 "full-width separators must not grow across the transition");

    const auto late =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.82
        );
    require(late.bands[0].identity_opacity > 0.9,
            "identity text must settle before terminal handoff");
    require(late.bands[0].character_opacity > 0.7,
            "characters must emerge after their realms");
    require(late.bands[0].card_opacity > 0.2,
            "cards must arrive during the final phase");
}

void test_frontier_starts_at_the_exact_captured_rune_edge() {
    const std::array<int, 4> ids{{1, 2, 3, 4}};
    const std::array<double, 4> heights{{270.0, 270.0, 270.0, 270.0}};
    const std::vector<WorkspaceMorphSource> sources{
        {1, {25.5, 310.0, 25.0, 31.0}, true, true},
    };
    const auto layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, sources, 1920.0, 1080.0
        );

    const auto hidden =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.0
        );
    require_near(hidden.bands[0].rune.x, 25.5,
                 "the proxy rune must not receive a synthetic x offset");
    require_near(hidden.bands[0].rune.y, 310.0,
                 "the proxy rune must preserve its live taskbar y position");
    require_near(hidden.bands[0].reveal_clip.x, 25.5,
                 "the hidden reveal must start at the exact rune left edge");
    require_near(hidden.bands[0].reveal_clip.width, 25.0,
                 "the hidden frontier must end at the exact rune right edge");
    require_near(hidden.bands[0].reveal_clip.y, 310.0,
                 "the hidden reveal must start at the exact rune top");
    require_near(hidden.bands[0].reveal_clip.height, 31.0,
                 "the hidden reveal must have the exact rune height");

    const auto ignition =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.06
        );
    require_near(ignition.bands[0].rune.x, hidden.bands[0].rune.x,
                 "ignition must not bounce the rune horizontally");
    require_near(ignition.bands[0].rune.y, hidden.bands[0].rune.y,
                 "ignition must not bounce the rune vertically");
    require(ignition.bands[0].rune_opacity > 0.9,
            "the rune must remain visible while its realm ignites");
    require_near(ignition.bands[0].reveal_clip.y, 310.0,
                 "early ignition must remain vertically local to the rune");
    require_near(ignition.bands[0].reveal_clip.height, 31.0,
                 "early ignition must not begin as a monitor-height curtain");
    require(ignition.bands[0].reveal_clip.width > 25.0 &&
            ignition.bands[0].reveal_clip.width < 90.0,
            "early ignition must breathe only a short distance from the rune");

    const auto unfolding =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.20
        );
    require(unfolding.bands[0].reveal_clip.height > 31.0,
            "the realm must grow vertically after the rune-local ignition");
    require(unfolding.bands[0].reveal_clip.height < 270.0,
            "vertical unfolding must remain incomplete before propagation settles");

    const auto propagating =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.34
        );
    require(propagating.bands[0].reveal_clip.x < 25.5,
            "the realm root must spread behind the taskbar after ignition");
    require(
        propagating.bands[0].reveal_clip.x +
            propagating.bands[0].reveal_clip.width > 160.0,
        "the frontier must propagate continuously from the rune seed"
    );
    require_near(propagating.bands[0].proxy_opacity, 0.0,
                 "no detached rail may appear between rune and frontier");
}


void test_close_topology_returns_to_the_rune_not_a_left_edge_line() {
    const std::array<int, 4> ids{{1, 2, 3, 4}};
    const std::array<double, 4> heights{{270.0, 270.0, 270.0, 270.0}};
    const std::vector<WorkspaceMorphSource> sources{
        {1, {18.0, 286.0, 25.0, 31.0}, true, true},
    };
    const auto layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, sources, 1920.0, 1080.0
        );

    const auto near_hidden =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.035
        );
    require(near_hidden.bands[0].reveal_clip.x >= 17.9,
            "late close must remain rooted at the workspace instead of screen zero");
    require(near_hidden.bands[0].reveal_clip.height < 40.0,
            "late close must shrink the frontier back to rune height");
    require(near_hidden.bands[0].seed_opacity > 0.0,
            "late close must retain a local absorption glow at the rune");

    const auto hidden =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.0
        );
    require_near(hidden.bands[0].reveal_clip.x, 18.0,
                 "close endpoint must terminate at the rune left edge");
    require_near(hidden.bands[0].reveal_clip.width, 25.0,
                 "close endpoint must terminate at the rune width");
    require_near(hidden.bands[0].seed_opacity, 0.0,
                 "the absorption glow must disappear exactly at the endpoint");
}


void test_rune_proxy_hands_off_without_a_visible_horizontal_rail() {
    const std::array<int, 4> ids{{1, 2, 3, 4}};
    const std::array<double, 4> heights{{270.0, 270.0, 270.0, 270.0}};
    const std::vector<WorkspaceMorphSource> sources{
        {1, {15.0, 220.0, 25.0, 31.0}, true, true},
    };
    const auto layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, sources, 1920.0, 1080.0
        );

    const auto ignition =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.02
        );
    require(ignition.bands[0].rune_opacity > 0.95,
            "ignition must begin with the accurate taskbar rune silhouette");
    require_near(ignition.bands[0].stroke_opacity, 0.0,
                 "the old rectangular proxy must not appear at ignition");
    require_near(ignition.bands[0].rune.width, 25.0,
                 "the proxy rune must preserve the captured artwork bounds");

    const auto crossover =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.13
        );
    require(crossover.bands[0].rune_opacity > 0.0,
            "the rune must still be visible during the direct handoff");
    require_near(crossover.bands[0].stroke_opacity, 0.0,
                 "the handoff must not draw a horizontal rail");

    const auto propagated =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 0.34
        );
    require_near(propagated.bands[0].rune_opacity, 0.0,
                 "the rune silhouette must be gone once propagation begins");
    require_near(propagated.bands[0].stroke_opacity, 0.0,
                 "propagation must remain free of horizontal proxy lines");
}

void test_rune_opacity_is_progress_only_and_reversible() {
    using realmheart::ui::workspace::animation::workspace_morph_rune_opacity;
    require_near(workspace_morph_rune_opacity(0.0), 1.0,
                 "taskbar rune artwork must be fully visible at hidden endpoint");
    require_near(workspace_morph_rune_opacity(1.0), 0.0,
                 "taskbar rune artwork must be hidden in the open overview");
    const double middle = workspace_morph_rune_opacity(0.13);
    require(middle > 0.0 && middle < 1.0,
            "taskbar rune artwork must crossfade during ignition");
    require_near(middle, workspace_morph_rune_opacity(0.13),
                 "reversal must sample the same rune opacity at the same progress");
}

void test_progress_is_clamped() {
    const std::array<int, 4> ids{{1, 2, 3, 4}};
    const std::array<double, 4> heights{{270.0, 270.0, 270.0, 270.0}};
    const auto layout =
        realmheart::ui::workspace::animation::build_workspace_morph_layout(
            ids, heights, {}, 1920.0, 1080.0
        );

    const auto below =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, -10.0
        );
    const auto above =
        realmheart::ui::workspace::animation::sample_workspace_morph_frame(
            layout, 10.0
        );
    require(below.exact_hidden, "negative progress must clamp hidden");
    require(above.exact_visible, "overshoot progress must clamp visible");
}

} // namespace

int main() {
    try {
        test_maps_sources_by_actual_workspace_id();
        test_missing_workspace_source_never_borrows_another_id();
        test_source_geometry_scaling_is_applied_once();
        test_active_and_occupied_state_do_not_change_bounds();
        test_destination_bands_preserve_exact_heights();
        test_hidden_and_visible_endpoints_are_exact();
        test_stages_overlap_without_early_cards();
        test_frontier_starts_at_the_exact_captured_rune_edge();
        test_close_topology_returns_to_the_rune_not_a_left_edge_line();
        test_rune_proxy_hands_off_without_a_visible_horizontal_rail();
        test_rune_opacity_is_progress_only_and_reversible();
        test_progress_is_clamped();
    } catch (const std::exception& error) {
        std::cerr << "WorkspaceMorphModelTests failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "Workspace morph model tests passed\n";
    return 0;
}
