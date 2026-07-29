#include "effects/core/ShaderPlayback.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

bool close_to(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.00001F;
}

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "ShaderPlaybackTests: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

int main() {
    using realmheart::effects::sample_shader_playback;

    const auto open_start = sample_shader_playback(0.0, true);
    require(close_to(open_start.progress, 0.0F), "open start progress");
    require(close_to(open_start.reverse, 1.0F), "open reverse flag");

    const auto close_start = sample_shader_playback(1.0, false);
    require(close_to(close_start.progress, 0.0F), "close start progress");
    require(close_to(close_start.reverse, 0.0F), "close reverse flag");

    const auto opening = sample_shader_playback(0.37, true);
    const auto closing = sample_shader_playback(0.37, false);
    require(close_to(opening.progress, 0.37F), "opening timeline mapping");
    require(close_to(closing.progress, 0.63F), "closing timeline mapping");

    // Both directions produce the same shader phase at a reversal point:
    // opening uses reverse=1 with progress=t; closing uses reverse=0 with
    // progress=1-t. This is the backend-neutral contract shared by GTK and the
    // future Hyprland window backend.
    const float opening_phase = 1.0F - opening.progress;
    const float closing_phase = closing.progress;
    require(close_to(opening_phase, closing_phase), "reversal continuity");

    const auto clamped_low = sample_shader_playback(-2.0, true);
    const auto clamped_high = sample_shader_playback(9.0, false);
    require(close_to(clamped_low.progress, 0.0F), "low clamp");
    require(close_to(clamped_high.progress, 0.0F), "high close clamp");

    return EXIT_SUCCESS;
}
