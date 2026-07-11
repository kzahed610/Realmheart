#pragma once

#include <gtk/gtk.h>
#include <functional>

namespace realmheart::ui::bar {

inline constexpr int kVerticalBarWidth = 72;

GtkWindow* present_vertical_bar(GtkApplication* application, std::function<void()> toggle_sidebar);

} // namespace realmheart::ui::bar
