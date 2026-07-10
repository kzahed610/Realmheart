#pragma once

#include <gtk/gtk.h>

namespace realmheart::ui::bar {

inline constexpr int kVerticalBarWidth = 72;

GtkWindow* present_vertical_bar(GtkApplication* application);

} // namespace realmheart::ui::bar
