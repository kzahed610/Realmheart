#include "ui/bar/VerticalBar.hpp"

#include "ui/AssetResolver.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/components/WorkspacePill.hpp"
#include "ui/components/ClockWidget.hpp"
#include "ui/components/StatusWidget.hpp"
#include "services/ThemeService.hpp"

namespace realmheart::ui::bar {

VerticalBar::VerticalBar(
    GtkApplication* app, 
    services::NotificationHistory& notification_history, 
    std::shared_ptr<services::ThemeService> theme_service,
    std::function<void()> toggle_sidebar
) : app_(app), notification_history_(notification_history), theme_service_(std::move(theme_service)), toggle_sidebar_(toggle_sidebar) {
    
    // Subscribe to shared theme changes
    theme_service_->subscribe([this](const services::Palette& palette) {
        for (auto& widget : widgets_) {
            if (auto themeable = std::dynamic_pointer_cast<components::ThemeableWidget>(widget)) {
                themeable->apply_theme(palette);
            }
        }
    });

    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Vertical Bar");
    gtk_window_set_default_size(GTK_WINDOW(window_), 50, 800);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    
    apply_layer_surface(GTK_WINDOW(window_), make_layer_surface_spec("realmheart-vertical-bar", LayerSurfaceLevel::Overlay, LayerKeyboardMode::OnDemand));
    
    setup_layout();
    populate_widgets();
    
    // Apply initial theme
    const auto& initial_palette = theme_service_->get_palette();
    for (auto& widget : widgets_) {
        if (auto themeable = std::dynamic_pointer_cast<components::ThemeableWidget>(widget)) {
            themeable->apply_theme(initial_palette);
        }
    }
}

void VerticalBar::setup_layout() {
    root_container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root_container_, "realmheart-vertical-bar");
    gtk_widget_set_size_request(root_container_, 50, -1);
    
    gtk_window_set_child(GTK_WINDOW(window_), root_container_);
}

void VerticalBar::populate_widgets() {
    // 1. Clock
    widgets_.push_back(std::make_shared<components::ClockWidget>());
    
    // 2. Workspace Pills
    // Normally we'd get these from a WorkspaceService, for now we use mocked state
    std::vector<components::WorkspaceState> mock_workspaces = {
        {1, "Web", true, 3},
        {2, "Dev", false, 5},
        {3, "Chat", false, 1}
    };
    
    for (const auto& state : mock_workspaces) {
        widgets_.push_back(std::make_shared<components::WorkspacePill>(state));
    }
    
    // 3. Status Slots
    std::vector<components::StatusWidget::Slot> slots = {
        {"Battery", "battery-full", "BAT", "Battery Level", "85%", true},
        {"Media", "media-play", "MED", "Now Playing", "Song.mp3", true},
        {"Notifications", "notification-bell", "NOT", "Notification Center", "3", true}
    };
    
    for (const auto& slot : slots) {
        widgets_.push_back(std::make_shared<components::StatusWidget>(slot, [this]() {
            toggle_sidebar_();
        }));
    }
}

void VerticalBar::refresh() {
    for (auto& widget : widgets_) widget->refresh();
}

GtkWidget* present_vertical_bar(GtkApplication* app, services::NotificationHistory& notification_history, std::shared_ptr<services::ThemeService> theme_service, std::function<void()> toggle_sidebar) {
    auto bar = std::make_unique<VerticalBar>(app, notification_history, theme_service, toggle_sidebar);
    return bar->get_window();
}

} // namespace realmheart::ui::bar
