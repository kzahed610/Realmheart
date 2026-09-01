#pragma once

namespace realmheart::ui {

class ShellState {
public:
    bool bar_visible() const { return bar_visible_; }
    bool right_sidebar_visible() const { return right_sidebar_visible_; }

    void show_bar() { bar_visible_ = true; }
    void toggle_bar() { bar_visible_ = !bar_visible_; }
    void toggle_right_sidebar() { right_sidebar_visible_ = !right_sidebar_visible_; }
    void set_right_sidebar_visible(bool visible) { right_sidebar_visible_ = visible; }

private:
    bool bar_visible_ = true;
    bool right_sidebar_visible_ = false;
};

} // namespace realmheart::ui
