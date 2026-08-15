#include "ui/bar/VerticalBar.hpp"

#include "core/TaskExecutor.hpp"
#include "services/HyprlandWorkspaces.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/bar/BarGeometry.hpp"
#include "ui/bar/VerticalBarModel.hpp"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>

namespace realmheart::ui::bar {
namespace {

int monitor_height_or_fallback(GtkWidget* widget) {
    constexpr int fallback_height = 1080;
    GdkMonitor* monitor = resolve_layer_surface_monitor(widget);
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

GtkWidget* create_section_separator(const char* role_class) {
    GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(separator, "realmheart-bar-separator");
    if (role_class != nullptr && *role_class != '\0') {
        gtk_widget_add_css_class(separator, role_class);
    }
    gtk_widget_set_halign(separator, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(separator, 28, 1);
    return separator;
}

} // namespace

VerticalBar::VerticalBar(
    GtkApplication* app,
    services::NotificationHistory& notification_history,
    services::BatteryService& battery_service,
    services::MediaService& media_service,
    std::function<void()> toggle_sidebar,
    std::function<void()> launch_launcher,
    std::function<void()> toggle_workspace_overview,
    std::function<void(double, double)> open_power_menu,
    std::function<void(services::WorkspaceSnapshot)> workspace_snapshot_changed
) : app_(app),
    notification_history_(notification_history),
    battery_service_(battery_service),
    media_service_(media_service),
    toggle_sidebar_(std::move(toggle_sidebar)),
    launch_launcher_(std::move(launch_launcher)),
    toggle_workspace_overview_(std::move(toggle_workspace_overview)),
    open_power_menu_(std::move(open_power_menu)),
    workspace_snapshot_changed_(std::move(workspace_snapshot_changed)) {
    g_weak_ref_init(&active_popover_ref_, nullptr);
    window_ = gtk_application_window_new(app_);
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Aether Spine");
    gtk_window_set_default_size(
        GTK_WINDOW(window_),
        kVisualWidth,
        monitor_height_or_fallback(window_)
    );
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    gtk_widget_add_css_class(window_, "realmheart-vertical-bar-window");
    gtk_widget_remove_css_class(window_, "background");

    // Windows begin at the straight 56 px rail. The curved caps deliberately
    // extend over their rounded top-left and bottom-left corner area.
    apply_layer_surface(GTK_WINDOW(window_), make_bar_surface_spec(kRailWidth));

    async_state_->owner = this;
    setup_layout();
    populate_widgets();

    const auto state = async_state_;
    notification_subscription_ = notification_history_.subscribe([state] {
        if (!state->alive.load() ||
            state->notification_refresh_queued.exchange(true)) return;
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* shared = static_cast<std::shared_ptr<AsyncState>*>(raw);
                (*shared)->notification_refresh_queued = false;
                if ((*shared)->alive.load() && (*shared)->owner != nullptr) {
                    (*shared)->owner->apply_notifications(
                        (*shared)->owner->notification_history_.snapshot()
                    );
                }
                return G_SOURCE_REMOVE;
            },
            new std::shared_ptr<AsyncState>(state),
            +[](gpointer raw) { delete static_cast<std::shared_ptr<AsyncState>*>(raw); }
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

    // Event-driven where possible. The only periodic I/O is a slow recovery
    // poll and cheap sysfs/nmcli state refresh; system usage is sampled only
    // while its popover is open.
    refresh_timer_id_ = g_timeout_add_seconds(5, +[](gpointer data) -> gboolean {
        auto* self = static_cast<VerticalBar*>(data);
        ++self->refresh_tick_;
        if (self->refresh_tick_ % 6 == 0) {
            self->request_media_refresh();
            self->request_battery_refresh();
            self->request_wifi_refresh();
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
    g_weak_ref_clear(&active_popover_ref_);

    bottom_action_button_.reset();
    notification_button_.reset();
    wifi_button_.reset();
    battery_widget_.reset();
    clock_.reset();
    workspace_runes_.clear();
    clear_box(workspace_box_);
    system_monitor_widget_.reset();
    media_widget_.reset();
    launcher_button_.reset();
    backdrop_.reset();

    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void VerticalBar::setup_layout() {
    root_overlay_ = gtk_overlay_new();
    gtk_widget_add_css_class(root_overlay_, "realmheart-bar-root");
    gtk_widget_set_hexpand(root_overlay_, TRUE);
    gtk_widget_set_vexpand(root_overlay_, TRUE);

    backdrop_ = std::make_unique<widgets::BarBackdrop>(
        GTK_WINDOW(window_),
        kRailWidth,
        kVisualWidth,
        kCurveHeight
    );
    gtk_overlay_set_child(GTK_OVERLAY(root_overlay_), backdrop_->widget());

    // A vertical GtkCenterBox gives the rail three deliberate zones:
    // identity/system controls at the top, workspaces in the visual center,
    // and clock/status/actions at the bottom. Each zone keeps compact internal
    // spacing without collapsing the entire UI into either edge.
    content_container_ = gtk_center_box_new();
    gtk_orientable_set_orientation(
        GTK_ORIENTABLE(content_container_),
        GTK_ORIENTATION_VERTICAL
    );
    gtk_widget_add_css_class(content_container_, "realmheart-vertical-bar");
    gtk_widget_set_size_request(content_container_, kRailWidth, -1);
    gtk_widget_set_halign(content_container_, GTK_ALIGN_START);
    gtk_widget_set_valign(content_container_, GTK_ALIGN_FILL);
    gtk_widget_set_vexpand(content_container_, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_overlay_), content_container_);

    workspace_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(workspace_box_, "realmheart-workspace-stack");
    gtk_widget_set_halign(workspace_box_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(workspace_box_, GTK_ALIGN_CENTER);

    GtkWidget* workspace_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(workspace_section, "realmheart-workspace-section");
    gtk_widget_set_halign(workspace_section, GTK_ALIGN_CENTER);
    gtk_box_append(
        GTK_BOX(workspace_section),
        create_section_separator("realmheart-workspace-separator")
    );
    gtk_box_append(GTK_BOX(workspace_section), workspace_box_);
    gtk_box_append(
        GTK_BOX(workspace_section),
        create_section_separator("realmheart-workspace-separator")
    );

    // Keep the workspace section in normal document flow. The previous
    // expanding GtkCenterBox consumed every spare pixel and created the huge
    // gaps above and below the runes.
    workspace_region_ = workspace_section;

    gtk_window_set_child(GTK_WINDOW(window_), root_overlay_);
}

std::pair<double, double> VerticalBar::power_menu_origin() const {
    constexpr double kFallbackOriginX = 24.0 / 1920.0;
    constexpr double kFallbackOriginY = 1048.0 / 1080.0;
    if (bottom_action_button_ == nullptr || window_ == nullptr) {
        return {kFallbackOriginX, kFallbackOriginY};
    }

    GtkWidget* button = bottom_action_button_->widget();
    const graphene_point_t centre = GRAPHENE_POINT_INIT(
        static_cast<float>(std::max(gtk_widget_get_width(button), 0)) * 0.5F,
        static_cast<float>(std::max(gtk_widget_get_height(button), 0)) * 0.5F
    );
    graphene_point_t in_bar{};
    if (!gtk_widget_compute_point(button, window_, &centre, &in_bar)) {
        return {kFallbackOriginX, kFallbackOriginY};
    }

    GdkMonitor* monitor = resolve_layer_surface_monitor(window_);
    if (monitor == nullptr) return {kFallbackOriginX, kFallbackOriginY};

    GdkRectangle geometry{};
    gdk_monitor_get_geometry(monitor, &geometry);
    g_object_unref(monitor);
    if (geometry.width <= 0 || geometry.height <= 0) {
        return {kFallbackOriginX, kFallbackOriginY};
    }

    return {
        std::clamp(static_cast<double>(in_bar.x) / geometry.width, 0.0, 1.0),
        std::clamp(static_cast<double>(in_bar.y) / geometry.height, 0.0, 1.0),
    };
}

void VerticalBar::populate_widgets() {
    auto exclusive_open = [this](GtkPopover* popover) { open_exclusive_popover(popover); };
    auto media_exclusive_open = [this] { open_exclusive_media(); };
    auto media_contour_occlusion = [this](int bottom_y) {
        if (backdrop_ != nullptr) backdrop_->set_top_contour_occlusion(bottom_y);
    };
    auto system_exclusive_open = [this] { open_exclusive_system(); };

    launcher_button_ = std::make_unique<widgets::BarIconButton>(
        "Realmheart-Icons/realmheart-launcher.svg",
        "RH",
        "Open Realmheart launcher",
        launch_launcher_
    );
    launcher_button_->add_css_class("realmheart-launcher-button");
    launcher_button_->set_icon_size(32);

    media_widget_ = std::make_unique<widgets::MediaWidget>(
        app_,
        media_service_,
        media_exclusive_open,
        media_contour_occlusion
    );
    system_monitor_widget_ = std::make_unique<widgets::SystemMonitorWidget>(
        app_, system_exclusive_open
    );
    clock_ = std::make_unique<widgets::ClockWidget>();
    battery_widget_ = std::make_unique<widgets::BatteryWidget>(exclusive_open);

    wifi_button_ = std::make_unique<widgets::BarIconButton>(
        "Realmheart-Icons/wifi.svg",
        "Wi",
        "Wi-Fi status",
        toggle_sidebar_
    );
    wifi_button_->add_css_class("realmheart-wifi-button");

    notification_button_ = std::make_unique<widgets::BarIconButton>(
        "Realmheart-Icons/notifications.svg",
        "Nt",
        "Notifications",
        toggle_sidebar_
    );
    notification_button_->add_css_class("realmheart-notification-button");

    bottom_action_button_ = std::make_unique<widgets::BarIconButton>(
        "Realmheart-Icons/power.svg",
        "Pw",
        "Open power menu",
        [this] {
            if (!open_power_menu_) return;
            const auto [origin_x, origin_y] = power_menu_origin();
            open_power_menu_(origin_x, origin_y);
        }
    );
    bottom_action_button_->add_css_class("realmheart-bottom-action-button");

    GtkWidget* top_cluster = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(top_cluster, "realmheart-bar-top-cluster");
    gtk_widget_set_halign(top_cluster, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(top_cluster), launcher_button_->widget());
    gtk_box_append(GTK_BOX(top_cluster), media_widget_->widget());
    gtk_box_append(GTK_BOX(top_cluster), system_monitor_widget_->widget());

    GtkWidget* bottom_cluster = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(bottom_cluster, "realmheart-bar-bottom-cluster");
    gtk_widget_set_halign(bottom_cluster, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bottom_cluster), clock_->widget());
    gtk_box_append(
        GTK_BOX(bottom_cluster),
        create_section_separator("realmheart-status-separator")
    );
    gtk_box_append(GTK_BOX(bottom_cluster), battery_widget_->widget());
    gtk_box_append(GTK_BOX(bottom_cluster), wifi_button_->widget());
    gtk_box_append(GTK_BOX(bottom_cluster), notification_button_->widget());

    // The final action belongs to the bottom module rather than living alone
    // in a distant corner. The whole module is pinned to the rail's bottom.
    gtk_widget_set_halign(bottom_action_button_->widget(), GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bottom_cluster), bottom_action_button_->widget());

    gtk_center_box_set_start_widget(GTK_CENTER_BOX(content_container_), top_cluster);
    gtk_center_box_set_center_widget(
        GTK_CENTER_BOX(content_container_),
        workspace_region_
    );
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(content_container_), bottom_cluster);

    // The media outside-click catcher deliberately leaves the physical bar
    // pointer-transparent so bar controls keep their normal one-click actions.
    // Close media during the bar's capture phase for every press except its own
    // icon; the original target then continues handling the same event.
    GtkGesture* bar_media_dismiss = gtk_gesture_click_new();
    gtk_gesture_single_set_button(
        GTK_GESTURE_SINGLE(bar_media_dismiss),
        GDK_BUTTON_PRIMARY
    );
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(bar_media_dismiss),
        GTK_PHASE_CAPTURE
    );
    g_signal_connect(
        bar_media_dismiss,
        "pressed",
        G_CALLBACK(+[](GtkGestureClick*, int, double x, double y, gpointer data) {
            auto* self = static_cast<VerticalBar*>(data);
            if (self->media_widget_ == nullptr || self->window_ == nullptr) return;

            GtkWidget* media_button = self->media_widget_->widget();
            GtkWidget* target = gtk_widget_pick(
                self->window_,
                x,
                y,
                GTK_PICK_DEFAULT
            );
            const bool pressed_media_icon = target != nullptr &&
                (target == media_button ||
                 gtk_widget_is_ancestor(target, media_button));
            if (!pressed_media_icon) self->media_widget_->close();
        }),
        this
    );
    gtk_widget_add_controller(
        window_,
        GTK_EVENT_CONTROLLER(bar_media_dismiss)
    );

    apply_notifications(notification_history_.snapshot());
}

void VerticalBar::open_exclusive_popover(GtkPopover* popover) {
    if (media_widget_ != nullptr) media_widget_->close();
    if (system_monitor_widget_ != nullptr) system_monitor_widget_->close();

    GObject* current = static_cast<GObject*>(g_weak_ref_get(&active_popover_ref_));
    if (current != nullptr && current != G_OBJECT(popover)) {
        gtk_popover_popdown(GTK_POPOVER(current));
    }
    g_clear_object(&current);
    g_weak_ref_set(&active_popover_ref_, G_OBJECT(popover));
}

void VerticalBar::open_exclusive_media() {
    if (system_monitor_widget_ != nullptr) system_monitor_widget_->close();

    GObject* current = static_cast<GObject*>(g_weak_ref_get(&active_popover_ref_));
    if (current != nullptr) {
        gtk_popover_popdown(GTK_POPOVER(current));
    }
    g_clear_object(&current);
    g_weak_ref_set(&active_popover_ref_, nullptr);
}

void VerticalBar::open_exclusive_system() {
    if (media_widget_ != nullptr) media_widget_->close();

    GObject* current = static_cast<GObject*>(g_weak_ref_get(&active_popover_ref_));
    if (current != nullptr) {
        gtk_popover_popdown(GTK_POPOVER(current));
    }
    g_clear_object(&current);
    g_weak_ref_set(&active_popover_ref_, nullptr);
}

void VerticalBar::request_workspace_overview_toggle() {
    if (media_widget_ != nullptr) media_widget_->close();
    if (system_monitor_widget_ != nullptr) system_monitor_widget_->close();

    GObject* current = static_cast<GObject*>(g_weak_ref_get(&active_popover_ref_));
    if (current != nullptr) {
        gtk_popover_popdown(GTK_POPOVER(current));
    }
    g_clear_object(&current);
    g_weak_ref_set(&active_popover_ref_, nullptr);

    if (toggle_workspace_overview_) toggle_workspace_overview_();
}

std::vector<workspace::animation::WorkspaceMorphSource>
VerticalBar::workspace_morph_sources() const {
    std::vector<workspace::animation::WorkspaceMorphSource> sources;
    sources.reserve(workspace_runes_.size());
    if (window_ == nullptr) return sources;

    for (const auto& rune : workspace_runes_) {
        if (rune == nullptr) continue;
        graphene_rect_t bounds{};
        if (!rune->compute_artwork_bounds(window_, &bounds)) continue;
        sources.push_back({
            rune->workspace_id(),
            {
                static_cast<double>(bounds.origin.x),
                static_cast<double>(bounds.origin.y),
                static_cast<double>(bounds.size.width),
                static_cast<double>(bounds.size.height),
            },
            rune->active(),
            rune->occupied(),
        });
    }
    return sources;
}

void VerticalBar::set_workspace_morph_active(bool active) {
    workspace_morph_active_ = active;
    // Hyprland hides normal Top-layer panels behind true fullscreen clients.
    // The workspace overview is also Top-layer, so temporarily lift the Aether
    // Spine to Overlay only while the overview owns the morph/visible state.
    // Returning to Top restores the bar's original fullscreen behavior.
    if (window_ != nullptr) {
        set_layer_surface_level(
            GTK_WINDOW(window_),
            active ? LayerSurfaceLevel::Overlay : LayerSurfaceLevel::Top
        );
    }
    if (!active) {
        set_workspace_morph_progress(0.0);
    }
    if (active) {
        if (media_widget_ != nullptr) media_widget_->close();
        if (system_monitor_widget_ != nullptr) system_monitor_widget_->close();

        GObject* current = static_cast<GObject*>(
            g_weak_ref_get(&active_popover_ref_)
        );
        if (current != nullptr) {
            gtk_popover_popdown(GTK_POPOVER(current));
        }
        g_clear_object(&current);
        g_weak_ref_set(&active_popover_ref_, nullptr);
    }

    for (const auto& rune : workspace_runes_) {
        if (rune != nullptr) rune->set_morph_suppressed(active);
    }
}


void VerticalBar::set_workspace_morph_progress(double progress) {
    workspace_morph_progress_ = std::clamp(progress, 0.0, 1.0);
    const double opacity =
        workspace::animation::workspace_morph_rune_opacity(
            workspace_morph_progress_
        );
    for (const auto& rune : workspace_runes_) {
        if (rune != nullptr) rune->set_morph_visual_opacity(opacity);
    }
}

void VerticalBar::activate_workspace(int workspace_id) {
    constexpr int kMinimumWorkspaceId = 1;
    if (workspace_id < kMinimumWorkspaceId) return;

    // Dispatch outside GTK's main loop. hyprctl normally returns quickly, but
    // a compositor IPC hiccup must never stall pointer handling or animation.
    const auto state = async_state_;
    static_cast<void>(realmheart::core::shared_task_executor().post([state, workspace_id] {
        if (!services::HyprlandWorkspaces::switch_to(workspace_id)) return;
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
    }));
}

void VerticalBar::request_workspace_refresh() {
    if (async_state_->workspace_in_flight.exchange(true)) {
        async_state_->workspace_refresh_pending = true;
        return;
    }
    const auto state = async_state_;
    if (!realmheart::core::shared_task_executor().post([state] {
        auto snapshot = services::HyprlandWorkspaces::read();
        struct Payload {
            std::shared_ptr<AsyncState> state;
            services::WorkspaceSnapshot snapshot;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                payload->state->workspace_in_flight = false;
                if (payload->state->alive.load() && payload->state->owner != nullptr) {
                    payload->state->owner->apply_workspaces(std::move(payload->snapshot));
                    if (payload->state->workspace_refresh_pending.exchange(false)) {
                        payload->state->owner->request_workspace_refresh();
                    }
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{state, std::move(snapshot)},
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    })) {
        state->workspace_in_flight = false;
    }
}

void VerticalBar::request_media_refresh() {
    if (async_state_->media_in_flight.exchange(true)) {
        async_state_->media_refresh_pending = true;
        return;
    }
    const auto state = async_state_;
    auto* service = &media_service_;
    if (!realmheart::core::shared_task_executor().post([state, service] {
        auto info = service->get_current_media();
        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::optional<services::MediaInfo> info;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                payload->state->media_in_flight = false;
                if (payload->state->alive.load() && payload->state->owner != nullptr) {
                    payload->state->owner->apply_media(payload->info);
                    if (payload->state->media_refresh_pending.exchange(false)) {
                        payload->state->owner->request_media_refresh();
                    }
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{state, std::move(info)},
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    })) {
        state->media_in_flight = false;
    }
}

void VerticalBar::request_battery_refresh() {
    if (async_state_->battery_in_flight.exchange(true)) return;
    const auto state = async_state_;
    auto* service = &battery_service_;
    if (!realmheart::core::shared_task_executor().post([state, service] {
        auto status = service->read();
        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::optional<services::BatteryStatus> status;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                payload->state->battery_in_flight = false;
                if (payload->state->alive.load() && payload->state->owner != nullptr) {
                    payload->state->owner->apply_battery(payload->status);
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{state, std::move(status)},
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    })) {
        state->battery_in_flight = false;
    }
}

void VerticalBar::request_wifi_refresh() {
    if (async_state_->wifi_in_flight.exchange(true)) return;
    const auto state = async_state_;
    if (!realmheart::core::shared_task_executor().post([state] {
        auto wifi = services::Wifi::read();
        struct Payload {
            std::shared_ptr<AsyncState> state;
            std::optional<services::WifiState> wifi;
        };
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<Payload*>(raw);
                payload->state->wifi_in_flight = false;
                if (payload->state->alive.load() && payload->state->owner != nullptr) {
                    payload->state->owner->apply_wifi(payload->wifi);
                }
                return G_SOURCE_REMOVE;
            },
            new Payload{state, std::move(wifi)},
            +[](gpointer raw) { delete static_cast<Payload*>(raw); }
        );
    })) {
        state->wifi_in_flight = false;
    }
}

void VerticalBar::apply_workspaces(services::WorkspaceSnapshot snapshot) {
    workspace_window_tracker_.apply(snapshot);
    if (workspace_snapshot_changed_) workspace_snapshot_changed_(snapshot);
    const auto states = build_workspace_pills(snapshot);
    const bool same_topology = states.size() == workspace_runes_.size() &&
        std::equal(states.begin(), states.end(), workspace_runes_.begin(), [](const auto& state, const auto& rune) {
            return state.id == rune->workspace_id();
        });

    if (same_topology) {
        for (std::size_t index = 0; index < states.size(); ++index) {
            workspace_runes_[index]->update(states[index]);
        }
        return;
    }

    workspace_runes_.clear();
    clear_box(workspace_box_);
    workspace_runes_.reserve(states.size());
    for (const auto& state : states) {
        auto rune = std::make_unique<widgets::WorkspaceRune>(
            state,
            [this](int workspace_id) { activate_workspace(workspace_id); },
            [this] { request_workspace_overview_toggle(); },
            [this](GtkPopover* popover) { open_exclusive_popover(popover); }
        );
        rune->set_morph_suppressed(workspace_morph_active_);
        rune->set_morph_visual_opacity(
            workspace::animation::workspace_morph_rune_opacity(
                workspace_morph_progress_
            )
        );
        gtk_box_append(GTK_BOX(workspace_box_), rune->widget());
        workspace_runes_.push_back(std::move(rune));
    }
}

void VerticalBar::apply_battery(const std::optional<services::BatteryStatus>& status) {
    battery_widget_->update(status);
}

void VerticalBar::apply_media(const std::optional<services::MediaInfo>& info) {
    media_widget_->update(info);
}

void VerticalBar::apply_wifi(const std::optional<services::WifiState>& state) {
    for (const char* css_class : {
        "realmheart-wifi-disconnected",
        "realmheart-wifi-weak",
        "realmheart-wifi-medium",
        "realmheart-wifi-strong",
    }) {
        wifi_button_->remove_css_class(css_class);
    }

    if (!state) {
        wifi_button_->set_icon("Realmheart-Icons/wifi-warning.svg", "Wi");
        wifi_button_->set_enabled(true);
        wifi_button_->add_css_class("realmheart-wifi-disconnected");
        wifi_button_->set_tooltip("Wi-Fi state unavailable");
        return;
    }

    if (!state->enabled) {
        wifi_button_->set_icon("Realmheart-Icons/wifi-off.svg", "Wi");
        wifi_button_->set_enabled(false);
        wifi_button_->add_css_class("realmheart-wifi-disconnected");
        wifi_button_->set_tooltip("Wi-Fi disabled");
        return;
    }

    if (state->ssid.empty()) {
        wifi_button_->set_icon("Realmheart-Icons/wifi-warning.svg", "Wi");
        wifi_button_->set_enabled(true);
        wifi_button_->add_css_class("realmheart-wifi-disconnected");
        wifi_button_->set_tooltip("Wi-Fi enabled — not connected");
        return;
    }

    const int strength = state->signal_percent.value_or(100);
    const char* icon = strength < 25
        ? "Realmheart-Icons/wifi-1.svg"
        : (strength < 50
            ? "Realmheart-Icons/wifi-2.svg"
            : (strength < 75
                ? "Realmheart-Icons/wifi-3.svg"
                : "Realmheart-Icons/wifi-4.svg"));
    wifi_button_->set_icon(icon, "Wi");
    wifi_button_->set_enabled(true);
    wifi_button_->add_css_class(
        strength < 35
            ? "realmheart-wifi-weak"
            : (strength < 70 ? "realmheart-wifi-medium" : "realmheart-wifi-strong")
    );
    wifi_button_->set_tooltip(
        "Wi-Fi: " + state->ssid +
        (state->signal_percent ? " (" + std::to_string(*state->signal_percent) + "%)" : "")
    );
}

void VerticalBar::apply_notifications(const services::NotificationSnapshot& notifications) {
    const bool unread = notifications.capture_active && notifications.unread_count > 0;
    notification_button_->set_enabled(unread);
    notification_button_->remove_css_class("realmheart-notification-unread");
    if (unread) notification_button_->add_css_class("realmheart-notification-unread");
    notification_button_->set_badge(
        unread ? std::to_string(notifications.unread_count) : std::string{}
    );
    notification_button_->set_tooltip(
        notifications.capture_active
            ? "Notifications: " + std::to_string(notifications.unread_count) + " unread"
            : "Notification capture unavailable"
    );
}

void VerticalBar::refresh() {
    clock_->refresh();
    request_workspace_refresh();
    request_media_refresh();
    request_battery_refresh();
    request_wifi_refresh();
    apply_notifications(notification_history_.snapshot());
}

} // namespace realmheart::ui::bar
