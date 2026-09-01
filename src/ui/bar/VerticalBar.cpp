#include "ui/bar/VerticalBar.hpp"

#include "core/TaskExecutor.hpp"
#include "services/HyprlandWorkspaces.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/bar/BarGeometry.hpp"
#include "ui/bar/VerticalBarModel.hpp"

#include <gtk4-layer-shell/gtk4-layer-shell.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>

namespace realmheart::ui::bar {
namespace {

void clear_box(GtkWidget* box) {
    GtkWidget* child = gtk_widget_get_first_child(box);
    while (child != nullptr) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(box), child);
        child = next;
    }
}

GtkWidget* create_section_separator(const char* role_class, int width) {
    GtkWidget* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(separator, "realmheart-bar-separator");
    if (role_class != nullptr && *role_class != '\0') {
        gtk_widget_add_css_class(separator, role_class);
    }
    gtk_widget_set_halign(separator, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(separator, width, 1);
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
    std::function<void(services::WorkspaceSnapshot)> workspace_snapshot_changed,
    int monitor_index
) : app_(app),
    monitor_index_(monitor_index),
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
        geometry_.visual_width,
        geometry_.surface_height
    );
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    gtk_widget_add_css_class(window_, "realmheart-vertical-bar-window");
    gtk_widget_remove_css_class(window_, "background");

    // Windows begin at the profile's straight rail. The curved caps deliberately
    // extend over their rounded top-left and bottom-left corner area.
    auto surface_spec = make_bar_surface_spec(geometry_.rail_width);
    surface_spec.monitor_index = monitor_index_;
    apply_layer_surface(GTK_WINDOW(window_), surface_spec);
    g_signal_connect(
        window_,
        "realize",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            static_cast<VerticalBar*>(data)->apply_geometry();
        }),
        this
    );

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
    if (geometry_retry_id_ != 0) {
        g_source_remove(geometry_retry_id_);
        geometry_retry_id_ = 0;
    }
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
    for (auto& rune : workspace_runes_) rune->begin_teardown();
    clear_box(workspace_runes_container_);
    workspace_runes_.clear();
    system_monitor_widget_.reset();
    media_widget_.reset();
    launcher_button_.reset();
    backdrop_.reset();

    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void VerticalBar::schedule_geometry_retry() {
    if (geometry_retry_id_ != 0 || window_ == nullptr) return;

    geometry_retry_id_ = g_timeout_add(
        50,
        +[](gpointer data) -> gboolean {
            auto* self = static_cast<VerticalBar*>(data);
            self->geometry_retry_id_ = 0;
            self->apply_geometry();
            return G_SOURCE_REMOVE;
        },
        this
    );
}

void VerticalBar::apply_geometry() {
    if (window_ == nullptr) return;

    GdkMonitor* monitor = resolve_layer_surface_monitor(window_, monitor_index_);
    if (monitor == nullptr) {
        schedule_geometry_retry();
        return;
    }

    GdkRectangle monitor_geometry{};
    gdk_monitor_get_geometry(monitor, &monitor_geometry);
    g_object_unref(monitor);
    if (monitor_geometry.width <= 0 || monitor_geometry.height <= 0) {
        schedule_geometry_retry();
        return;
    }

    const BarGeometry next_geometry = bar_geometry_for_logical_geometry(
        monitor_geometry.width,
        monitor_geometry.height
    );
    geometry_ = next_geometry;
    apply_layout_metrics();

    gtk_window_set_default_size(
        GTK_WINDOW(window_),
        geometry_.visual_width,
        geometry_.surface_height
    );
    if (content_container_ != nullptr) {
        gtk_widget_set_size_request(
            content_container_,
            geometry_.rail_width,
            geometry_.surface_height
        );
        gtk_widget_queue_resize(content_container_);
    }
    gtk_layer_set_exclusive_zone(
        GTK_WINDOW(window_),
        geometry_.rail_width
    );
    gtk_widget_queue_resize(window_);
}

void VerticalBar::apply_layout_metrics() {
    if (backdrop_ != nullptr) {
        backdrop_->set_geometry(
            geometry_.rail_width,
            geometry_.visual_width,
            geometry_.curve_height
        );
    }

    if (content_container_ != nullptr) {
        gtk_widget_set_size_request(
            content_container_, geometry_.rail_width, geometry_.surface_height
        );

        // GtkWidget has margin setters but no runtime padding setter. Margin is
        // not equivalent here: it moves the entire rail child instead of
        // preserving the released inner content box. Select one local CSS tier
        // class so the authored 1080p padding stays byte-for-byte equivalent.
        for (const char* css_class : {
            "realmheart-bar-tier-1080p",
            "realmheart-bar-tier-1440p",
            "realmheart-bar-tier-4k",
        }) {
            gtk_widget_remove_css_class(content_container_, css_class);
        }
        switch (geometry_.display_tier) {
        case core::DisplayTier::P1440:
            gtk_widget_add_css_class(content_container_, "realmheart-bar-tier-1440p");
            break;
        case core::DisplayTier::P4K:
            gtk_widget_add_css_class(content_container_, "realmheart-bar-tier-4k");
            break;
        case core::DisplayTier::P1080:
        default:
            gtk_widget_add_css_class(content_container_, "realmheart-bar-tier-1080p");
            break;
        }
    }

    if (top_cluster_ != nullptr) {
        gtk_box_set_spacing(GTK_BOX(top_cluster_), geometry_.top_cluster_spacing);
        gtk_widget_set_margin_top(top_cluster_, geometry_.top_cluster_margin_top);
    }
    if (bottom_cluster_ != nullptr) {
        gtk_box_set_spacing(GTK_BOX(bottom_cluster_), geometry_.bottom_cluster_spacing);
        gtk_widget_set_margin_bottom(
            bottom_cluster_, geometry_.bottom_cluster_margin_bottom
        );
    }
    if (workspace_region_ != nullptr) {
        gtk_box_set_spacing(GTK_BOX(workspace_region_), geometry_.workspace_section_spacing);
    }
    if (workspace_box_ != nullptr) {
        gtk_widget_set_size_request(workspace_box_, geometry_.workspace_stack_width, -1);
    }
    if (workspace_runes_container_ != nullptr) {
        gtk_widget_set_margin_top(
            workspace_runes_container_, geometry_.workspace_stack_padding
        );
        gtk_widget_set_margin_bottom(
            workspace_runes_container_, geometry_.workspace_stack_padding
        );
        gtk_box_set_spacing(
            GTK_BOX(workspace_runes_container_), geometry_.workspace_stack_spacing
        );
    }
    for (GtkWidget* separator : {
        workspace_top_separator_, workspace_bottom_separator_, status_separator_
    }) {
        if (separator != nullptr) {
            gtk_widget_set_size_request(separator, geometry_.separator_width, 1);
        }
    }
    if (status_separator_ != nullptr) {
        gtk_widget_set_margin_bottom(
            status_separator_, geometry_.status_separator_bottom_margin
        );
    }
    if (notification_button_ != nullptr) {
        gtk_widget_set_margin_bottom(
            notification_button_->widget(), geometry_.notification_bottom_margin
        );
    }
    if (bottom_action_button_ != nullptr) {
        gtk_widget_set_margin_bottom(
            bottom_action_button_->widget(), geometry_.bottom_action_bottom_margin
        );
    }

    if (launcher_button_ != nullptr) {
        launcher_button_->set_layout(
            geometry_.icon_button_extent, geometry_.launcher_icon_size
        );
    }
    if (media_widget_ != nullptr) media_widget_->set_layout(geometry_);
    if (system_monitor_widget_ != nullptr) system_monitor_widget_->set_layout(geometry_);
    if (battery_widget_ != nullptr) battery_widget_->set_layout(geometry_);
    for (const auto& button : {
        wifi_button_.get(), notification_button_.get(), bottom_action_button_.get()
    }) {
        if (button != nullptr) {
            button->set_layout(geometry_.icon_button_extent, geometry_.icon_size);
        }
    }
    for (const auto& rune : workspace_runes_) {
        if (rune != nullptr) rune->set_layout(geometry_);
    }
}

void VerticalBar::setup_layout() {
    root_overlay_ = gtk_overlay_new();
    gtk_widget_add_css_class(root_overlay_, "realmheart-bar-root");
    gtk_widget_set_hexpand(root_overlay_, TRUE);
    gtk_widget_set_vexpand(root_overlay_, TRUE);

    backdrop_ = std::make_unique<widgets::BarBackdrop>(
        GTK_WINDOW(window_),
        geometry_.rail_width,
        geometry_.visual_width,
        geometry_.curve_height
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
    gtk_widget_add_css_class(content_container_, "realmheart-bar-tier-1080p");
    gtk_widget_set_size_request(
        content_container_,
        geometry_.rail_width,
        geometry_.surface_height
    );
    gtk_widget_set_halign(content_container_, GTK_ALIGN_START);
    gtk_widget_set_valign(content_container_, GTK_ALIGN_FILL);
    gtk_widget_set_vexpand(content_container_, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(root_overlay_), content_container_);

    workspace_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(workspace_box_, "realmheart-workspace-stack");
    gtk_widget_set_halign(workspace_box_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(workspace_box_, GTK_ALIGN_CENTER);
    workspace_runes_container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(workspace_runes_container_, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(workspace_box_), workspace_runes_container_);

    // The pill itself is the right-click target for the workspace overview,
    // not the individual runes. GTK delivers events over a widget's padding
    // to the widget itself, so the whole capsule - separators, spacing, and
    // runes alike - toggles the overview. Blind clicks welcome.
    GtkGesture* pill_right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(
        GTK_GESTURE_SINGLE(pill_right_click),
        GDK_BUTTON_SECONDARY
    );
    g_signal_connect(pill_right_click, "pressed", G_CALLBACK(+[](
        GtkGestureClick*, int, double, double, gpointer data
    ) {
        auto* bar = static_cast<VerticalBar*>(data);
        if (bar->workspace_morph_active_) return;
        bar->request_workspace_overview_toggle();
    }), this);
    gtk_widget_add_controller(workspace_box_, GTK_EVENT_CONTROLLER(pill_right_click));

    workspace_region_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(workspace_region_, "realmheart-workspace-section");
    gtk_widget_set_halign(workspace_region_, GTK_ALIGN_CENTER);
    workspace_top_separator_ = create_section_separator(
        "realmheart-workspace-separator", geometry_.separator_width
    );
    workspace_bottom_separator_ = create_section_separator(
        "realmheart-workspace-separator", geometry_.separator_width
    );
    gtk_box_append(
        GTK_BOX(workspace_region_), workspace_top_separator_
    );
    gtk_box_append(GTK_BOX(workspace_region_), workspace_box_);
    gtk_box_append(
        GTK_BOX(workspace_region_), workspace_bottom_separator_
    );

    // Keep the workspace section in normal document flow. The previous
    // expanding GtkCenterBox consumed every spare pixel and created the huge
    // gaps above and below the runes.
    gtk_window_set_child(GTK_WINDOW(window_), root_overlay_);
}

std::string VerticalBar::assigned_monitor_connector() const {
    if (window_ == nullptr || !gtk_widget_get_realized(window_)) return {};
    GdkMonitor* monitor = resolve_layer_surface_monitor(window_, monitor_index_);
    if (monitor == nullptr) return {};
    const char* connector = gdk_monitor_get_connector(monitor);
    std::string result = connector != nullptr ? connector : "";
    g_object_unref(monitor);
    return result;
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

    GdkMonitor* monitor = resolve_layer_surface_monitor(window_, monitor_index_);
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

    media_widget_ = std::make_unique<widgets::MediaWidget>(
        app_,
        media_service_,
        media_exclusive_open,
        media_contour_occlusion,
        monitor_index_
    );
    system_monitor_widget_ = std::make_unique<widgets::SystemMonitorWidget>(
        app_, system_exclusive_open, monitor_index_
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

    top_cluster_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(top_cluster_, "realmheart-bar-top-cluster");
    gtk_widget_set_halign(top_cluster_, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(top_cluster_), launcher_button_->widget());
    gtk_box_append(GTK_BOX(top_cluster_), media_widget_->widget());
    gtk_box_append(GTK_BOX(top_cluster_), system_monitor_widget_->widget());

    bottom_cluster_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(bottom_cluster_, "realmheart-bar-bottom-cluster");
    gtk_widget_set_halign(bottom_cluster_, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bottom_cluster_), clock_->widget());
    status_separator_ = create_section_separator(
        "realmheart-status-separator", geometry_.separator_width
    );
    gtk_box_append(GTK_BOX(bottom_cluster_), status_separator_);
    gtk_box_append(GTK_BOX(bottom_cluster_), battery_widget_->widget());
    gtk_box_append(GTK_BOX(bottom_cluster_), wifi_button_->widget());
    gtk_box_append(GTK_BOX(bottom_cluster_), notification_button_->widget());

    // The final action belongs to the bottom module rather than living alone
    // in a distant corner. The whole module is pinned to the rail's bottom.
    gtk_widget_set_halign(bottom_action_button_->widget(), GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bottom_cluster_), bottom_action_button_->widget());

    gtk_center_box_set_start_widget(GTK_CENTER_BOX(content_container_), top_cluster_);
    gtk_center_box_set_center_widget(
        GTK_CENTER_BOX(content_container_),
        workspace_region_
    );
    gtk_center_box_set_end_widget(GTK_CENTER_BOX(content_container_), bottom_cluster_);

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

    apply_layout_metrics();
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

    // Dispatch outside GTK's main loop. Pin the workspace switch to this bar's
    // output so clicking a secondary-monitor rune cannot mutate whichever
    // output happened to be focused a few milliseconds earlier.
    const auto state = async_state_;
    const std::string monitor = assigned_monitor_connector();
    static_cast<void>(realmheart::core::shared_task_executor().post([
        state,
        workspace_id,
        monitor
    ] {
        const bool switched = monitor.empty()
            ? services::HyprlandWorkspaces::switch_to(workspace_id)
            : services::HyprlandWorkspaces::switch_to_on_monitor(
                  workspace_id, monitor
              );
        if (!switched) return;
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
    const std::string monitor = assigned_monitor_connector();
    if (!realmheart::core::shared_task_executor().post([state, monitor] {
        auto snapshot = monitor.empty()
            ? services::HyprlandWorkspaces::read()
            : services::HyprlandWorkspaces::read_for_monitor(monitor);
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

    // Teardown order matters: stop each rune's timers and arm its guard
    // while the objects are alive, then unparent the buttons (GTK emits
    // synthesized crossing events during unmap — those must hit live,
    // guarded objects, not freed ones), and only then destroy the runes.
    for (auto& rune : workspace_runes_) rune->begin_teardown();
    clear_box(workspace_runes_container_);
    workspace_runes_.clear();
    workspace_runes_.reserve(states.size());
    for (const auto& state : states) {
        auto rune = std::make_unique<widgets::WorkspaceRune>(
            state,
            [this](int workspace_id) { activate_workspace(workspace_id); },
            [this](GtkPopover* popover) { open_exclusive_popover(popover); }
        );
        rune->set_layout(geometry_);
        rune->set_morph_suppressed(workspace_morph_active_);
        rune->set_morph_visual_opacity(
            workspace::animation::workspace_morph_rune_opacity(
                workspace_morph_progress_
            )
        );
        gtk_box_append(GTK_BOX(workspace_runes_container_), rune->widget());
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
