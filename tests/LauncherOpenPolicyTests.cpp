#include "ui/launcher/LauncherOpenPolicy.hpp"
#include "ui/launcher/LauncherPointerSelection.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_search_results_reveal_during_open_transition() {
    using realmheart::ui::launcher::should_reveal_search_results;

    require(
        should_reveal_search_results(true, true),
        "active query results should reveal while the opening transition targets visible"
    );
    require(
        !should_reveal_search_results(true, false),
        "active query results should stay hidden while the launcher targets hidden"
    );
    require(
        !should_reveal_search_results(false, true),
        "empty search should not reveal query results"
    );
}

void test_query_open_skips_idle_constellation_refresh() {
    using realmheart::ui::launcher::OpenIntent;
    using realmheart::ui::launcher::should_refresh_idle_content;

    require(
        should_refresh_idle_content(OpenIntent::Browse),
        "browse opens should continue to build idle constellation content"
    );
    require(
        !should_refresh_idle_content(OpenIntent::Query),
        "query opens should skip idle constellation work"
    );
}

void test_query_open_keeps_fast_results_reveal_duration() {
    using realmheart::ui::launcher::OpenIntent;
    using realmheart::ui::launcher::results_reveal_duration_ms;

    require(
        results_reveal_duration_ms(OpenIntent::Browse, 220, 80) == 220,
        "ordinary mode entry should retain its established reveal duration"
    );
    require(
        results_reveal_duration_ms(OpenIntent::Query, 220, 80) == 80,
        "query-open mode setup should not overwrite the fast reveal duration"
    );
}

void test_special_picker_starts_at_first_row_until_pointer_moves() {
    using realmheart::ui::launcher::should_select_result_row_for_pointer_motion;

    require(
        !should_select_result_row_for_pointer_motion(
            true, false, 0.0, 0.0, 320.0, 240.0, 0.5
        ),
        "the first pointer sample must not override the picker's first-row selection"
    );
    require(
        !should_select_result_row_for_pointer_motion(
            true, true, 320.0, 240.0, 320.2, 240.2, 0.5
        ),
        "stationary-pointer noise must not change the selected picker row"
    );
    require(
        should_select_result_row_for_pointer_motion(
            true, true, 320.0, 240.0, 324.0, 240.0, 0.5
        ),
        "real pointer movement must select the row under the cursor"
    );
    require(
        should_select_result_row_for_pointer_motion(
            false, false, 0.0, 0.0, 320.0, 240.0, 0.5
        ),
        "ordinary launcher results must retain their initial hover behavior"
    );
}

} // namespace

int main() {
    test_search_results_reveal_during_open_transition();
    test_query_open_skips_idle_constellation_refresh();
    test_query_open_keeps_fast_results_reveal_duration();
    test_special_picker_starts_at_first_row_until_pointer_moves();
    std::cout << "Launcher open policy tests passed\n";
    return 0;
}
