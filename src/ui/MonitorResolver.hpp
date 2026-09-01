#pragma once

#include "core/MonitorContext.hpp"

#include <gtk/gtk.h>

#include <optional>
#include <string>

namespace realmheart::ui {

[[nodiscard]] int monitor_count(GdkDisplay* display) noexcept;

[[nodiscard]] std::optional<core::MonitorContext> monitor_context_for_index(
    GdkDisplay* display,
    int monitor_index
);

[[nodiscard]] std::optional<core::MonitorContext> monitor_context_for_widget(
    GtkWidget* widget,
    int monitor_index = -1
);

// Resolves Hyprland's focused output back to GDK's monitor model by connector
// name. Falls back to REALMHEART_MONITOR_INDEX / monitor 0 when focus metadata
// is unavailable.
[[nodiscard]] int focused_monitor_index(GdkDisplay* display);

[[nodiscard]] std::string monitor_connector_for_index(
    GdkDisplay* display,
    int monitor_index
);

} // namespace realmheart::ui
