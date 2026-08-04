#include "ui/powermenu/PowerMenuVideoState.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

using realmheart::ui::powermenu::PowerMenuVideoPhase;
using realmheart::ui::powermenu::PowerMenuVideoState;

void asset_tree_contains_exactly_one_valid_mp4() {
    const std::filesystem::path root(REALMHEART_TEST_POWER_MENU_ROOT);
    std::error_code error;
    assert(std::filesystem::is_directory(root, error));
    assert(!error);

    std::size_t entry_count = 0U;
    std::filesystem::path video;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        ++entry_count;
        video = entry.path();
        assert(entry.is_regular_file());
    }
    assert(entry_count == 1U);
    assert(video.filename() == "realmheart-power-menu.mp4");
    assert(std::filesystem::file_size(video) > 1'000'000U);

    std::ifstream input(video, std::ios::binary);
    std::array<char, 12> header{};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    assert(input.gcount() == static_cast<std::streamsize>(header.size()));
    assert(std::string_view(header.data() + 4, 4) == "ftyp");
}

void starts_hidden_without_media_or_frames() {
    PowerMenuVideoState state;

    assert(state.phase() == PowerMenuVideoPhase::Hidden);
    assert(state.opacity() == 0.0);
    assert(!state.media_required());
    assert(!state.needs_frame());
}

void present_acquires_media_and_completes_the_opening_transition() {
    PowerMenuVideoState state;

    state.present();
    assert(state.phase() == PowerMenuVideoPhase::Opening);
    assert(state.media_required());
    assert(state.needs_frame());

    state.advance(0.20);
    assert(state.opacity() > 0.0 && state.opacity() < 1.0);

    state.advance(1.0);
    assert(state.phase() == PowerMenuVideoPhase::Visible);
    assert(std::abs(state.opacity() - 1.0) < 0.0001);
    assert(state.media_required());
    assert(!state.needs_frame());
}

void dismiss_keeps_media_through_fade_then_releases_it_at_hidden() {
    PowerMenuVideoState state;
    state.present();
    state.advance(1.0);

    state.dismiss();
    assert(state.phase() == PowerMenuVideoPhase::Closing);
    assert(state.media_required());
    assert(state.needs_frame());

    state.advance(0.10);
    assert(state.opacity() > 0.0 && state.opacity() < 1.0);
    assert(state.media_required());

    state.advance(1.0);
    assert(state.phase() == PowerMenuVideoPhase::Hidden);
    assert(state.opacity() == 0.0);
    assert(!state.media_required());
    assert(!state.needs_frame());
}

void reversing_a_close_is_continuous_and_never_drops_media() {
    PowerMenuVideoState state;
    state.present();
    state.advance(0.20);
    const double opening_opacity = state.opacity();

    state.dismiss();
    state.advance(0.05);
    const double closing_opacity = state.opacity();
    assert(closing_opacity < opening_opacity);
    assert(state.media_required());

    state.present();
    assert(state.phase() == PowerMenuVideoPhase::Opening);
    assert(std::abs(state.opacity() - closing_opacity) < 0.0001);
    assert(state.media_required());

    state.advance(1.0);
    assert(state.phase() == PowerMenuVideoPhase::Visible);
    assert(state.opacity() == 1.0);
}

void immediate_hide_terminates_every_lifecycle_obligation() {
    PowerMenuVideoState state;
    state.present();
    state.advance(0.15);
    state.hide_immediately();

    assert(state.phase() == PowerMenuVideoPhase::Hidden);
    assert(state.opacity() == 0.0);
    assert(!state.media_required());
    assert(!state.needs_frame());
}

} // namespace

int main() {
    asset_tree_contains_exactly_one_valid_mp4();
    starts_hidden_without_media_or_frames();
    present_acquires_media_and_completes_the_opening_transition();
    dismiss_keeps_media_through_fade_then_releases_it_at_hidden();
    reversing_a_close_is_continuous_and_never_drops_media();
    immediate_hide_terminates_every_lifecycle_obligation();
    std::cout << "Power menu video state tests passed\n";
    return 0;
}
