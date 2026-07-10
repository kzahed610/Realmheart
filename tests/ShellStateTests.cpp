#include "ui/ShellState.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        realmheart::ui::ShellState state;
        require(state.bar_visible(), "bar should start visible");
        require(!state.right_sidebar_visible(), "right sidebar should start hidden");

        state.toggle_bar();
        require(!state.bar_visible(), "bar toggle should hide the bar");

        state.toggle_right_sidebar();
        require(state.right_sidebar_visible(), "sidebar toggle should show the sidebar");
        require(!state.bar_visible(), "sidebar toggle must not change bar visibility");

        state.show_bar();
        require(state.bar_visible(), "activation should be able to show the bar");
    } catch (const std::exception& error) {
        std::cerr << "ShellStateTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "ShellStateTests passed\n";
    return 0;
}
