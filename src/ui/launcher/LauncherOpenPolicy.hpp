#pragma once

namespace realmheart::ui::launcher {

enum class OpenIntent {
    Browse,
    Query,
};

[[nodiscard]] constexpr bool should_refresh_idle_content(OpenIntent intent) noexcept {
    return intent == OpenIntent::Browse;
}

[[nodiscard]] constexpr unsigned int results_reveal_duration_ms(
    OpenIntent intent,
    unsigned int browse_duration_ms,
    unsigned int query_duration_ms
) noexcept {
    return intent == OpenIntent::Query
        ? query_duration_ms
        : browse_duration_ms;
}

[[nodiscard]] constexpr bool should_reveal_search_results(
    bool searching,
    bool transition_targets_visible
) noexcept {
    return searching && transition_targets_visible;
}

} // namespace realmheart::ui::launcher
