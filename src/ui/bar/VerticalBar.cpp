#include "ui/bar/VerticalBar.hpp"

#include "core/TaskExecutor.hpp"
#include "services/HyprlandWorkspaces.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/bar/VerticalBarModel.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace realmheart::ui::bar {
namespace {

int monitor_height_or_fallback() {
    constexpr int fallback_height = 1080;
    GdkDisplay* display = gdk_display_get_default();
    if (display == nullptr) return fallback_height;
    GListModel* monitors = gdk_display_get_monitors(display);
    if (monitors == nullptr || g_list_model_get_n_items(monitors) == 0) return fallback_height;
    GdkMonitor* monitor = GDK_MONITOR(g_list_model_get_item(monitors, 0));
    if (monitor == nullptr) return fallback_height;
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
    apply_layer_surface(GTK_WINDOW(window_), make_bar_surface_spec(50));

    async_state_->owner = this;
    setup_layout();
    populate_widgets();

    const auto state = async_state_;
    notification_subscription_ = notification_history_.subscribe([state](const auto& snapshot) {
        if (!state->alive.load()) return;
        struct Payload {
            std::shared_ptr<AsyncState> state;
            services::NotificationSnapshot snapshot;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                if (payload->state->alive.load() && payload->state->owner != nullptr) {
                    payload->state->owner->apply_notifications(payload->snapshot);
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{state, snapshot},
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    });

    media_subscription_ = media_service_.subscribe([state] {
        if (!state->alive.load()) return;
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* shared = static_cast<std::shared_ptr<AsyncState>*>(raw);
                if ((*shared)->alive.load() && (*shared)->owner != nullptr) {
                    (*shared)->owner->request_media_refresh();
                }
                return G_SOURCE_REMOVE;
            },
            new std::shared_ptr<AsyncState>(state),
            +[](gpointer raw) { delete static_cast<std::shared_ptr<AsyncState>*>(raw); }
        );
    });

    workspace_monitor_ = std::make_unique<services::HyprlandEventMonitor>([state] {
        if (!state->alive.load()) return;
        // request_workspace_refresh() is thread-safe, but routing through GTK's
        // context keeps all scheduling/lifetime decisions on the UI thread.
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* shared = static_cast<std::shared_ptr<AsyncState>*>(raw);
                if ((*shared)->alive.load() && (*shared)->owner != nullptr) {
                    (*shared)->owner->request_workspace_refresh();
                }
                return G_SOURCE_REMOVE;
            },
            new std::shared_ptr<AsyncState>(state),
            +[](gpointer raw) { delete static_cast<std::shared_ptr<AsyncState>*>(raw); }
        );
    });
    workspace_monitor_->start();

    // Workspaces and media are event-driven. These slow recovery polls cover
    // compositor/session-bus reconnects; battery reads remain cheap sysfs I/O.
    refresh_timer_id_ = g_timeout_add_seconds(5, +[](gpointer data) -> gboolean {
        auto* self = static_cast<VerticalBar*>(data);
        ++self->refresh_tick_;
        if (self->refresh_tick_ % 6 == 0) {
            self->request_media_refresh();
            self->request_battery_refresh();
        }
        if (self->refresh_tick_ % 12 == 0) self->request_workspace_refresh();
        return G_SOURCE_CONTINUE;
    }, this);
    refresh();
}

VerticalBar::~VerticalBar() {
    if (refresh_timer_id_ != 0) {
        g_source_remove(refresh_timer_id_);
        refresh_timer_id_ = 0;
    }
    notification_subscription_.reset();
    media_subscription_.reset();
    async_state_->alive = false;
    workspace_monitor_.reset();
    async_state_->owner = nullptr;
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
    gtk_box_append(GTK_BOX(root_container_), status_box_);

    battery_status_ = std::make_unique<components::StatusWidget>(
        components::StatusWidget::Slot{"Battery", "battery-charging.svg", "Bt", "Battery pending", {}, false},
        toggle_sidebar_
    );
    media_status_ = std::make_unique<components::StatusWidget>(
        components::StatusWidget::Slot{"Media", "music-note.svg", "Md", "Media pending", {}, false},
        toggle_sidebar_
    );
    notification_status_ = std::make_unique<components::StatusWidget>(
        components::StatusWidget::Slot{"Notifications", "alert.svg", "Nt", "Notifications pending", {}, false},
        toggle_sidebar_
    );
    gtk_box_append(GTK_BOX(status_box_), battery_status_->get_widget());
    gtk_box_append(GTK_BOX(status_box_), media_status_->get_widget());
    gtk_box_append(GTK_BOX(status_box_), notification_status_->get_widget());
    apply_notifications(notification_history_.snapshot());
}

void VerticalBar::request_workspace_refresh() {
    if (async_state_->workspace_in_flight.exchange(true)) {
        async_state_->workspace_refresh_pending = true;
        return;
    }
    const auto state = async_state_;
    if (!realmheart::core::shared_task_executor().post([state] {
        auto snapshot = services::HyprlandWorkspaces::read();
        struct Payload { std::shared_ptr<AsyncState> state; services::WorkspaceSnapshot snapshot; };
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer raw) -> gboolean {
            auto* payload = static_cast<Payload*>(raw);
            payload->state->workspace_in_flight = false;
            if (payload->state->alive.load() && payload->state->owner != nullptr) {
                payload->state->owner->apply_workspaces(payload->snapshot);
                if (payload->state->workspace_refresh_pending.exchange(false)) {
                    payload->state->owner->request_workspace_refresh();
                }
            }
            return G_SOURCE_REMOVE;
        }, new Payload{state, std::move(snapshot)}, +[](gpointer raw) { delete static_cast<Payload*>(raw); });
    })) {
        state->workspace_in_flight = false;
    }
}

void VerticalBar::request_media_refresh() {
    if (async_state_->media_in_flight.exchange(true)) return;
    const auto state = async_state_;
    auto* service = &media_service_;
    realmheart::core::shared_task_executor().post([state, service] {
        auto info = service->get_current_media();
        struct Payload { std::shared_ptr<AsyncState> state; std::optional<services::MediaInfo> info; };
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer raw) -> gboolean {
            auto* payload = static_cast<Payload*>(raw);
            payload->state->media_in_flight = false;
            if (payload->state->alive.load() && payload->state->owner != nullptr) {
                payload->state->owner->apply_media(payload->info);
            }
            return G_SOURCE_REMOVE;
        }, new Payload{state, std::move(info)}, +[](gpointer raw) { delete static_cast<Payload*>(raw); });
    });
}

void VerticalBar::request_battery_refresh() {
    if (async_state_->battery_in_flight.exchange(true)) return;
    const auto state = async_state_;
    auto* service = &battery_service_;
    realmheart::core::shared_task_executor().post([state, service] {
        auto status = service->read();
        struct Payload { std::shared_ptr<AsyncState> state; std::optional<services::BatteryStatus> status; };
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer raw) -> gboolean {
            auto* payload = static_cast<Payload*>(raw);
            payload->state->battery_in_flight = false;
            if (payload->state->alive.load() && payload->state->owner != nullptr) {
                payload->state->owner->apply_battery(payload->status);
            }
            return G_SOURCE_REMOVE;
        }, new Payload{state, std::move(status)}, +[](gpointer raw) { delete static_cast<Payload*>(raw); });
    });
}

void VerticalBar::apply_workspaces(const services::WorkspaceSnapshot& snapshot) {
    const auto states = build_workspace_pills(snapshot);
    const bool same_topology = states.size() == workspace_pills_.size() &&
        std::equal(states.begin(), states.end(), workspace_pills_.begin(), [](const auto& state, const auto& pill) {
            return state.id == pill->workspace_id();
        });

    if (same_topology) {
        for (std::size_t index = 0; index < states.size(); ++index) {
            workspace_pills_[index]->update(states[index]);
        }
        return;
    }

    workspace_pills_.clear();
    clear_box(workspace_box_);
    workspace_pills_.reserve(states.size());
    for (const auto& state : states) {
        auto pill = std::make_unique<components::WorkspacePill>(state);
        gtk_box_append(GTK_BOX(workspace_box_), pill->get_widget());
        workspace_pills_.push_back(std::move(pill));
    }
}

void VerticalBar::apply_battery(const std::optional<services::BatteryStatus>& status) {
    components::StatusWidget::Slot slot{"Battery", "battery-charging.svg", "Bt", "Battery unavailable", {}, false};
    if (status) {
        slot.enabled = true;
        slot.badge_text = std::to_string(status->percentage) + "%";
        slot.tooltip = "Battery: " + slot.badge_text + " (" + status->status + ")";
    }
    battery_status_->set_status(slot);
}

void VerticalBar::apply_media(const std::optional<services::MediaInfo>& info) {
    components::StatusWidget::Slot slot{"Media", "music-note.svg", "Md", "No active media player", {}, false};
    if (info) {
        slot.enabled = info->playback_status != 0;
        slot.tooltip = "Media: " + info->artist + " — " + info->title;
        slot.badge_text = info->playback_status == 1 ? "▶" : "Ⅱ";
    }
    media_status_->set_status(slot);
}

void VerticalBar::apply_notifications(const services::NotificationSnapshot& notifications) {
    components::StatusWidget::Slot slot{
        "Notifications", "alert.svg", "Nt",
        notifications.capture_active
            ? "Notifications: " + std::to_string(notifications.unread_count) + " unread"
            : "Notifications: capture unavailable",
        notifications.unread_count > 0 ? std::to_string(notifications.unread_count) : std::string{},
        notifications.unread_count > 0
    };
    notification_status_->set_status(slot);
}

void VerticalBar::refresh() {
    clock_->refresh();
    request_workspace_refresh();
    request_media_refresh();
    request_battery_refresh();
    apply_notifications(notification_history_.snapshot());
}

} // namespace realmheart::ui::bar
