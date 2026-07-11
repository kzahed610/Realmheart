#pragma once

#include <gtk/gtk.h>
#include <functional>

namespace realmheart::services {
class NotificationHistory;
}

namespace realmheart::ui::bar {

inline constexpr int kVerticalBarWidth = 72;

GtkWindow* present_vertical_bar(
    GtkApplication* application,
    services::NotificationHistory& notification_history,
    std::function<void()> toggle_sidebar
);

} // namespace realmheart::ui::bar
