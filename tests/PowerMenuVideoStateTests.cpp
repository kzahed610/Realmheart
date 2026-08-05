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

void asset_tree_contains_video_and_static_poster() {
    const std::filesystem::path root(REALMHEART_TEST_POWER_MENU_ROOT);
    std::error_code error;
    assert(std::filesystem::is_directory(root, error));
    assert(!error);

    std::filesystem::path video;
    std::filesystem::path poster;
    std::size_t entry_count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        assert(entry.is_regular_file());
        ++entry_count;
        if (entry.path().filename() == "realmheart-power-menu.mp4") {
            video = entry.path();
        } else if (entry.path().filename() == "realmheart-power-menu-poster.jpg") {
            poster = entry.path();
        }
    }

    assert(entry_count == 2U);
    assert(!video.empty());
    assert(!poster.empty());
    assert(std::filesystem::file_size(video) > 1'000'000U);
    assert(std::filesystem::file_size(poster) > 100'000U);

    std::ifstream video_input(video, std::ios::binary);
    std::array<char, 12> video_header{};
    video_input.read(
        video_header.data(),
        static_cast<std::streamsize>(video_header.size())
    );
    assert(video_input.gcount() == static_cast<std::streamsize>(video_header.size()));
    assert(std::string_view(video_header.data() + 4, 4) == "ftyp");

    std::ifstream poster_input(poster, std::ios::binary);
    std::array<unsigned char, 3> poster_header{};
    poster_input.read(
        reinterpret_cast<char*>(poster_header.data()),
        static_cast<std::streamsize>(poster_header.size())
    );
    assert(poster_input.gcount() == static_cast<std::streamsize>(poster_header.size()));
    assert(poster_header[0] == 0xffU);
    assert(poster_header[1] == 0xd8U);
    assert(poster_header[2] == 0xffU);
}

void starts_hidden_without_media_or_frames() {
    PowerMenuVideoState state;

    assert(state.phase() == PowerMenuVideoPhase::Hidden);
    assert(state.progress() == 0.0);
    assert(state.opacity() == 0.0);
    assert(state.controls_opacity() == 0.0);
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
    assert(state.progress() > 0.0 && state.progress() < 1.0);
    assert(state.opacity() > 0.0 && state.opacity() < 1.0);
    assert(state.controls_opacity() == 0.0);

    state.advance(0.80);
    assert(state.controls_opacity() > 0.0 && state.controls_opacity() < 1.0);

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
    state.advance(0.80);
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
    asset_tree_contains_video_and_static_poster();
    starts_hidden_without_media_or_frames();
    present_acquires_media_and_completes_the_opening_transition();
    dismiss_keeps_media_through_fade_then_releases_it_at_hidden();
    reversing_a_close_is_continuous_and_never_drops_media();
    immediate_hide_terminates_every_lifecycle_obligation();
    std::cout << "Power menu video state tests passed\n";
    return 0;
}
