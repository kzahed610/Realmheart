#include "ui/bar/VerticalBar.hpp"

#include "services/HyprlandWorkspaces.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/bar/VerticalBarModel.hpp"

#include <string>
#include <utility>

namespace realmheart::ui::bar {
namespace {

int monitor_height_or_fallback() {
    constexpr int fallback_height = 1080;

    GdkDisplay* display = gdk_display_get_default();
    if (display == nullptr) {
        return fallback_height;
    }

    GListModel* monitors = gdk_display_get_monitors(display);
    if (monitors == nullptr || g_list_model_get_n_items(monitors) == 0) {
        return fallback_height;
    }

    GdkMonitor* monitor = GDK_MONITOR(g_list_model_get_item(monitors, 0));
    if (monitor == nullptr) {
        return fallback_height;
    }

    GdkRectangle geometry{};
    gdk_monitor_get_geometry(monitor, &geometry);
    g_object_unref(monitor);

    return geometry.height > 0 ? geometry.height : fallback_height;
}

void clear_box(GtkWidget* box) {
    GtkWidget* child = gtk_widget_get_first_child(box);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

} // namespace

VerticalBar::VerticalBar(
    GtkApplication* app,
    services::NotificationHistory& notification_history,
    services::BatteryService& battery_service,
    services::MediaService& media_service,
    std::function<void()> toggle_sidebar
) : app_(app),
    notification_history_(notification_history),
    battery_service_(battery_service),
    media_service_(media_service),
    toggle_sidebar_(std::move(toggle_sidebar)) {
    window_ = gtk_application_window_new(app_);
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Vertical Bar");
    gtk_window_set_default_size(GTK_WINDOW(window_), 50, monitor_height_or_fallback());
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    gtk_widget_add_css_class(window_, "realmheart-vertical-bar-window");

    apply_layer_surface(
        GTK_WINDOW(window_),
        make_bar_surface_spec(50)
    );

    setup_layout();
    populate_widgets();
}

VerticalBar::~VerticalBar() {
    notification_status_.reset();
    media_status_.reset();
    battery_status_.reset();
    workspace_pills_.clear();
    clock_.reset();
    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void VerticalBar::setup_layout() {
    root_container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root_container_, "realmheart-vertical-bar");
    gtk_widget_set_vexpand(root_container_, TRUE);
    gtk_widget_set_valign(root_container_, GTK_ALIGN_FILL);
    gtk_widget_set_halign(root_container_, GTK_ALIGN_FILL);
    gtk_widget_set_size_request(root_container_, 50, -1);

    workspace_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(workspace_box_, TRUE);
    gtk_widget_set_valign(workspace_box_, GTK_ALIGN_START);

    status_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(status_box_, GTK_ALIGN_END);

    gtk_window_set_child(GTK_WINDOW(window_), root_container_);
}

void VerticalBar::populate_widgets() {
    clock_ = std::make_unique<components::ClockWidget>();
    gtk_box_append(GTK_BOX(root_container_), clock_->get_widget());

    gtk_box_append(GTK_BOX(root_container_), workspace_box_);
    rebuild_workspaces();

    gtk_box_append(GTK_BOX(root_container_), status_box_);

    battery_status_ = std::make_unique<components::StatusWidget>(
        components::StatusWidget::Slot{
            "Battery",
            "battery-charging.svg",
            "Bt",
            "Battery status pending",
            {},
            false
        },
        toggle_sidebar_
    );
    media_status_ = std::make_unique<components::StatusWidget>(
        components::StatusWidget::Slot{
            "Media",
            "music-note.svg",
            "Md",
            "Media status pending",
            {},
            false
        },
        toggle_sidebar_
    );
    notification_status_ = std::make_unique<components::StatusWidget>(
        components::StatusWidget::Slot{
            "Notifications",
            "alert.svg",
            "Nt",
            "Notifications pending",
            {},
            false
        },
        toggle_sidebar_
    );

    gtk_box_append(GTK_BOX(status_box_), battery_status_->get_widget());
    gtk_box_append(GTK_BOX(status_box_), media_status_->get_widget());
    gtk_box_append(GTK_BOX(status_box_), notification_status_->get_widget());
    refresh_statuses();
}

void VerticalBar::rebuild_workspaces() {
    // Release C++ callbacks/controllers while their GTK widgets are still alive,
    // then remove the widget tree from the container.
    workspace_pills_.clear();
    clear_box(workspace_box_);

    const auto states = build_workspace_pills(services::HyprlandWorkspaces::read());
    workspace_pills_.reserve(states.size());
    for (const auto& state : states) {
        auto pill = std::make_unique<components::WorkspacePill>(state);
        gtk_box_append(GTK_BOX(workspace_box_), pill->get_widget());
        workspace_pills_.push_back(std::move(pill));
    }
}

void VerticalBar::refresh_statuses() {
    components::StatusWidget::Slot battery{
        "Battery",
        "battery-charging.svg",
        "Bt",
        "Battery unavailable",
        {},
        false
    };
    if (const auto status = battery_service_.read()) {
        battery.enabled = true;
        battery.badge_text = std::to_string(status->percentage) + "%";
        battery.tooltip = "Battery: " + battery.badge_text + " (" + status->status + ")";
    }
    battery_status_->set_status(battery);

    components::StatusWidget::Slot media{
        "Media",
        "music-note.svg",
        "Md",
        "No active media player",
        {},
        false
    };
    if (const auto info = media_service_.get_current_media()) {
        media.enabled = info->playback_status != 0;
        media.tooltip = "Media: " + info->artist + " — " + info->title;
        media.badge_text = info->playback_status == 1 ? "▶" : "Ⅱ";
    }
    media_status_->set_status(media);

    const auto notifications = notification_history_.snapshot();
    components::StatusWidget::Slot notification{
        "Notifications",
        "alert.svg",
        "Nt",
        notifications.capture_active
            ? "Notifications: " + std::to_string(notifications.unread_count) + " unread"
            : "Notifications: capture unavailable",
        notifications.unread_count > 0
            ? std::to_string(notifications.unread_count)
            : std::string{},
        notifications.unread_count > 0
    };
    notification_status_->set_status(notification);
}

void VerticalBar::refresh() {
    clock_->refresh();
    rebuild_workspaces();
    refresh_statuses();
}

} // namespace realmheart::ui::bar
