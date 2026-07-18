#include "ui/sidebar/RightSidebar.hpp"

#include "ui/sidebar/ConnectivityPanel.hpp"
#include "ui/sidebar/NightLightPanel.hpp"
#include "ui/sidebar/SidebarFrame.hpp"

#include "core/TaskExecutor.hpp"
#include "services/Audio.hpp"
#include "services/Bluetooth.hpp"
#include "services/Brightness.hpp"
#include "services/NightLight.hpp"
#include "services/PowerProfiles.hpp"
#include "services/Wifi.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"
#include "ui/components/NotificationWidget.hpp"
#include "ui/components/SliderWidget.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>

namespace realmheart::ui::sidebar {
namespace {

constexpr double kSidebarHeightFraction = 0.90;
constexpr int kSidebarRightMargin = 2;

GtkWidget* themed_icon(const char* path, int pixels, const char* css_class = nullptr) {
    realmheart::ui::bar::widgets::ThemedSvgIcon icon(path, pixels);
    if (css_class != nullptr) icon.add_css_class(css_class);
    return icon.widget();
}

GtkWidget* left_label(const char* text, const char* css_class = nullptr) {
    GtkWidget* label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    if (css_class != nullptr) gtk_widget_add_css_class(label, css_class);
    return label;
}

std::string uptime_text() {
    std::ifstream input("/proc/uptime");
    double uptime_seconds = 0.0;
    if (!(input >> uptime_seconds) || uptime_seconds < 0.0) return "Uptime unavailable";

    const auto total_minutes = static_cast<long long>(uptime_seconds / 60.0);
    const auto days = total_minutes / (24 * 60);
    const auto hours = (total_minutes / 60) % 24;
    const auto minutes = total_minutes % 60;

    std::ostringstream result;
    result << "Uptime  ";
    if (days > 0) result << days << "d ";
    if (hours > 0 || days > 0) result << hours << "h ";
    result << minutes << "m";
    return result.str();
}

} // namespace

class QuickControlTile {
public:
    QuickControlTile(
        const char* label,
        const char* icon_path,
        std::function<void()> activated
    ) : activated_(std::move(activated)) {
        button_ = gtk_button_new();
        gtk_widget_add_css_class(button_, "realmheart-quick-tile");
        gtk_widget_set_hexpand(button_, TRUE);
        gtk_widget_set_size_request(button_, -1, 64);

        GtkWidget* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_halign(content, GTK_ALIGN_FILL);
        gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(content), themed_icon(
            icon_path, 21, "realmheart-quick-tile-icon"
        ));

        GtkWidget* copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand(copy, TRUE);
        gtk_widget_set_halign(copy, GTK_ALIGN_FILL);
        GtkWidget* title = gtk_label_new(label);
        gtk_widget_add_css_class(title, "realmheart-quick-tile-title");
        gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
        gtk_box_append(GTK_BOX(copy), title);
        status_ = gtk_label_new("Unavailable");
        gtk_widget_add_css_class(status_, "realmheart-quick-tile-status");
        gtk_label_set_xalign(GTK_LABEL(status_), 0.0F);
        gtk_label_set_ellipsize(GTK_LABEL(status_), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(status_), 13);
        gtk_box_append(GTK_BOX(copy), status_);
        gtk_box_append(GTK_BOX(content), copy);
        gtk_button_set_child(GTK_BUTTON(button_), content);

        g_signal_connect(button_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<QuickControlTile*>(data);
            if (self->activated_) self->activated_();
        }), this);
    }

    GtkWidget* widget() const { return button_; }

    void set_state(std::string status, bool active, bool available = true) {
        gtk_label_set_text(GTK_LABEL(status_), status.c_str());
        gtk_widget_remove_css_class(button_, "active");
        gtk_widget_remove_css_class(button_, "unavailable");
        if (active) gtk_widget_add_css_class(button_, "active");
        if (!available) gtk_widget_add_css_class(button_, "unavailable");
        gtk_widget_set_sensitive(button_, available);
    }

private:
    GtkWidget* button_ = nullptr;
    GtkWidget* status_ = nullptr;
    std::function<void()> activated_;
};

SidebarPlacement sidebar_placement_for(GtkWidget* widget) {
    SidebarPlacement placement;
    GdkDisplay* display = gtk_widget_get_display(widget);
    if (display == nullptr) return placement;

    GListModel* monitors = gdk_display_get_monitors(display);
    if (monitors == nullptr || g_list_model_get_n_items(monitors) == 0) {
        return placement;
    }

    auto* monitor = GDK_MONITOR(g_list_model_get_item(monitors, 0));
    if (monitor == nullptr) return placement;

    GdkRectangle geometry{};
    gdk_monitor_get_geometry(monitor, &geometry);
    g_object_unref(monitor);

    if (geometry.height <= 0) return placement;

    placement.height = std::max(
        static_cast<int>(std::lround(
            static_cast<double>(geometry.height) * kSidebarHeightFraction
        )),
        1
    );
    placement.top_margin = std::max((geometry.height - placement.height) / 2, 0);
    return placement;
}

RightSidebar::RightSidebar(
    GtkApplication* app,
    services::NotificationHistory& notification_history,
    std::function<void(double)> show_volume_osd,
    std::function<void(double)> show_brightness_osd
) : app_(app),
    keep_awake_(std::make_shared<services::KeepAwake>()),
    notification_history_(notification_history),
    show_volume_osd_(std::move(show_volume_osd)),
    show_brightness_osd_(std::move(show_brightness_osd)) {
    async_ui_state_->owner = this;
    window_ = gtk_application_window_new(app_);
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Right Sidebar");
    gtk_widget_add_css_class(window_, "realmheart-sidebar-window");
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);

    const auto placement = sidebar_placement_for(window_);
    gtk_window_set_default_size(
        GTK_WINDOW(window_),
        kDefaultSidebarFrameLayout.surface_width(),
        placement.height
    );

    auto layer_spec = make_layer_surface_spec(
        "realmheart-right-sidebar",
        LayerSurfaceLevel::Overlay,
        LayerKeyboardMode::OnDemand
    );
    layer_spec.anchor_bottom = false;
    layer_spec.margin_top = placement.top_margin;
    layer_spec.margin_right = kSidebarRightMargin;
    apply_layer_surface(GTK_WINDOW(window_), layer_spec);

    setup_layout();
    populate_modules();
    refresh_controls();
}

RightSidebar::~RightSidebar() {
    async_ui_state_->alive = false;
    async_ui_state_->owner = nullptr;
    wifi_panel_.reset();
    bluetooth_panel_.reset();
    night_light_panel_.reset();
    modules_.clear();
    wifi_tile_.reset();
    bluetooth_tile_.reset();
    night_light_tile_.reset();
    keep_awake_tile_.reset();
    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void RightSidebar::setup_layout() {
    frame_ = std::make_unique<SidebarFrame>(
        GTK_WINDOW(window_), kDefaultSidebarFrameLayout
    );

    content_overlay_ = gtk_overlay_new();
    gtk_widget_add_css_class(content_overlay_, "realmheart-sidebar-content-overlay");
    gtk_widget_set_hexpand(content_overlay_, TRUE);
    gtk_widget_set_vexpand(content_overlay_, TRUE);

    container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(container_, "realmheart-right-sidebar");
    gtk_widget_set_hexpand(container_, TRUE);
    gtk_widget_set_vexpand(container_, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(content_overlay_), container_);

    frame_->set_child(content_overlay_);
    gtk_window_set_child(GTK_WINDOW(window_), frame_->widget());
}

void RightSidebar::build_identity_header() {
    GtkWidget* header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class(header, "realmheart-identity-header");

    GtkWidget* title = gtk_label_new("REALMHEART");
    gtk_widget_add_css_class(title, "realmheart-identity-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.5F);
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_halign(title, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(header), title);

    GtkWidget* state_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(state_row, "realmheart-identity-state-row");
    GtkWidget* subtitle = left_label(
        "Zahed  •  CachyOS", "realmheart-identity-subtitle"
    );
    gtk_widget_set_hexpand(subtitle, TRUE);
    gtk_box_append(GTK_BOX(state_row), subtitle);
    online_label_ = left_label("●  ONLINE", "realmheart-online-state");
    gtk_box_append(GTK_BOX(state_row), online_label_);
    uptime_label_ = left_label("Uptime", "realmheart-uptime");
    gtk_box_append(GTK_BOX(state_row), uptime_label_);
    gtk_box_append(GTK_BOX(header), state_row);
    gtk_box_append(GTK_BOX(container_), header);
}

void RightSidebar::build_quick_controls() {
    GtkWidget* section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(section, "realmheart-sidebar-section");
    GtkWidget* heading = left_label("QUICK CONTROLS", "realmheart-section-title");
    gtk_box_append(GTK_BOX(section), heading);

    GtkWidget* grid = gtk_grid_new();
    gtk_widget_add_css_class(grid, "realmheart-quick-grid");
    gtk_grid_set_column_spacing(GTK_GRID(grid), 7);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 7);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);

    wifi_tile_ = std::make_unique<QuickControlTile>(
        "Wi-Fi", "Realmheart-Icons/wifi.svg", [this] {
            if (bluetooth_panel_) bluetooth_panel_->hide();
            if (night_light_panel_) night_light_panel_->hide();
            if (wifi_panel_) wifi_panel_->toggle();
        }
    );
    bluetooth_tile_ = std::make_unique<QuickControlTile>(
        "Bluetooth", "Realmheart-Icons/bluetooth.svg", [this] {
            if (wifi_panel_) wifi_panel_->hide();
            if (night_light_panel_) night_light_panel_->hide();
            if (bluetooth_panel_) bluetooth_panel_->toggle();
        }
    );
    night_light_tile_ = std::make_unique<QuickControlTile>(
        "Night Light", "Realmheart-Icons/night-light.svg", [this] {
            if (wifi_panel_) wifi_panel_->hide();
            if (bluetooth_panel_) bluetooth_panel_->hide();
            if (night_light_panel_) night_light_panel_->toggle();
        }
    );
    keep_awake_tile_ = std::make_unique<QuickControlTile>(
        "Keep Awake", "Realmheart-Icons/keep-awake.svg", [this] {
            const bool enabled = !keep_awake_->active();
            post_control_action([keep_awake = keep_awake_, enabled] {
                static_cast<void>(keep_awake->set_enabled(enabled));
            });
        }
    );

    gtk_grid_attach(GTK_GRID(grid), wifi_tile_->widget(), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bluetooth_tile_->widget(), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), night_light_tile_->widget(), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), keep_awake_tile_->widget(), 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(section), grid);
    gtk_box_append(GTK_BOX(container_), section);

    wifi_panel_ = std::make_unique<WifiManagerPopover>(
        content_overlay_, [this] { refresh_controls(); }
    );
    bluetooth_panel_ = std::make_unique<BluetoothManagerPopover>(
        content_overlay_, [this] { refresh_controls(); }
    );
    night_light_panel_ = std::make_unique<NightLightPanel>(
        content_overlay_, [this] { refresh_controls(); }
    );
}

void RightSidebar::build_power_profiles() {
    GtkWidget* section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(section, "realmheart-sidebar-section");
    gtk_widget_add_css_class(section, "realmheart-power-section");
    gtk_box_append(GTK_BOX(section), left_label(
        "POWER PROFILE", "realmheart-section-title"
    ));

    GtkWidget* segments = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(segments, "realmheart-power-segments");
    constexpr std::array<const char*, 3> profiles{
        "power-saver", "balanced", "performance"
    };
    constexpr std::array<const char*, 3> labels{
        "Power", "Balanced", "Performance"
    };
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        GtkWidget* button = gtk_button_new_with_label(labels[index]);
        gtk_widget_add_css_class(button, "realmheart-power-segment");
        gtk_widget_set_hexpand(button, TRUE);
        g_object_set_data_full(
            G_OBJECT(button), "realmheart-profile",
            g_strdup(profiles[index]), g_free
        );
        g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton* pressed, gpointer data) {
            auto* self = static_cast<RightSidebar*>(data);
            const char* profile = static_cast<const char*>(g_object_get_data(
                G_OBJECT(pressed), "realmheart-profile"
            ));
            if (profile != nullptr) self->set_power_profile(profile);
        }), this);
        power_profile_buttons_[index] = button;
        gtk_box_append(GTK_BOX(segments), button);
    }
    gtk_box_append(GTK_BOX(section), segments);
    gtk_box_append(GTK_BOX(container_), section);
}

void RightSidebar::populate_modules() {
    build_identity_header();
    build_quick_controls();

    GtkWidget* slider_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(slider_section, "realmheart-sidebar-section");
    gtk_widget_add_css_class(slider_section, "realmheart-levels-section");
    gtk_box_append(GTK_BOX(slider_section), left_label(
        "SYSTEM LEVELS", "realmheart-section-title"
    ));

    GtkWidget* sliders = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(sliders, "realmheart-slider-chamber");

    auto brightness_widget = std::make_unique<components::SliderWidget>(
        "Brightness",
        0,
        100,
        0.0,
        [](double value) {
            const auto mutation = services::Brightness::set_percent(
                static_cast<int>(std::lround(value))
            );
            if (!mutation.success) return std::optional<double>{};
            return std::optional<double>{mutation.state.percent};
        },
        show_brightness_osd_
    );
    brightness_slider_ = brightness_widget.get();
    brightness_slider_->set_available(false);
    gtk_box_append(GTK_BOX(sliders), brightness_widget->get_widget());
    modules_.push_back(std::move(brightness_widget));

    auto volume_widget = std::make_unique<components::SliderWidget>(
        "Volume",
        0,
        150,
        0.0,
        [](double value) {
            const auto mutation = services::Audio::set_default_sink_volume(value / 100.0);
            if (!mutation.success) return std::optional<double>{};
            return std::optional<double>{mutation.state.volume * 100.0};
        },
        show_volume_osd_
    );
    volume_slider_ = volume_widget.get();
    volume_slider_->set_available(false);
    gtk_box_append(GTK_BOX(sliders), volume_widget->get_widget());
    modules_.push_back(std::move(volume_widget));

    gtk_box_append(GTK_BOX(slider_section), sliders);
    gtk_box_append(GTK_BOX(container_), slider_section);

    build_power_profiles();

    auto notifications = std::make_unique<components::NotificationWidget>(
        notification_history_
    );
    GtkWidget* notification_widget = notifications->get_widget();
    gtk_widget_set_vexpand(notification_widget, FALSE);
    gtk_box_append(GTK_BOX(container_), notification_widget);
    modules_.push_back(std::move(notifications));
}

void RightSidebar::refresh() {
    refresh_controls();
}

void RightSidebar::refresh_controls() {
    // Keep the open path GTK-only and cheap. NotificationWidget already tracks
    // history changes through its subscription, so rebuilding every row here
    // would only delay presentation of the sidebar.
    const bool online = g_network_monitor_get_network_available(
        g_network_monitor_get_default()
    );
    gtk_label_set_text(GTK_LABEL(online_label_), online ? "●  ONLINE" : "●  OFFLINE");
    gtk_widget_remove_css_class(online_label_, "offline");
    if (!online) gtk_widget_add_css_class(online_label_, "offline");
    gtk_label_set_text(GTK_LABEL(uptime_label_), uptime_text().c_str());
    keep_awake_tile_->set_state(
        keep_awake_->active() ? "Enabled" : "Disabled",
        keep_awake_->active(),
        true
    );

    const auto state = async_ui_state_;
    if (state->refresh_in_flight.exchange(true)) {
        state->refresh_pending = true;
        return;
    }

    const bool queued = realmheart::core::shared_task_executor().post([state] {
        std::optional<services::WifiState> wifi;
        std::optional<services::BluetoothState> bluetooth;
        std::optional<services::NightLightState> night_light;
        std::optional<std::string> active_profile;
        std::optional<services::BrightnessState> brightness;
        std::optional<services::AudioState> audio;
        {
            // Serialize reads with mutations so the UI never receives a
            // half-updated state, while keeping every subprocess off GTK's
            // main thread.
            std::lock_guard lock(state->operation_mutex);
            if (!state->alive.load()) {
                state->refresh_in_flight = false;
                return;
            }
            wifi = services::Wifi::read();
            bluetooth = services::Bluetooth::read();
            night_light = services::NightLight::read();
            active_profile = services::PowerProfiles::current();
            brightness = services::Brightness::read();
            audio = services::Audio::read_default_sink();
        }

        auto* result = new ControlRefreshResult{
            .state = state,
            .wifi_status = std::nullopt,
            .wifi_active = false,
            .bluetooth_powered = bluetooth
                ? std::optional<bool>{bluetooth->powered}
                : std::nullopt,
            .night_light_enabled = night_light
                ? std::optional<bool>{night_light->enabled}
                : std::nullopt,
            .active_profile = std::move(active_profile),
            .brightness_percent = brightness
                ? std::optional<double>{brightness->percent}
                : std::nullopt,
            .volume_percent = audio
                ? std::optional<double>{audio->volume * 100.0}
                : std::nullopt,
        };
        if (wifi) {
            result->wifi_active = wifi->enabled;
            result->wifi_status = !wifi->enabled
                ? "Off"
                : (wifi->ssid.empty() ? "Disconnected" : wifi->ssid);
        }

        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            &RightSidebar::finish_control_refresh,
            result,
            &RightSidebar::destroy_control_refresh_result
        );
    });
    if (!queued) state->refresh_in_flight = false;
}

gboolean RightSidebar::finish_control_refresh(gpointer raw) {
    auto* result = static_cast<ControlRefreshResult*>(raw);
    auto& state = *result->state;
    auto* owner = state.owner;
    if (state.alive.load() && owner != nullptr) {
        if (result->wifi_status) {
            owner->wifi_tile_->set_state(
                *result->wifi_status, result->wifi_active, true
            );
        } else {
            owner->wifi_tile_->set_state("Unavailable", false, false);
        }

        if (result->bluetooth_powered) {
            owner->bluetooth_tile_->set_state(
                *result->bluetooth_powered ? "On" : "Off",
                *result->bluetooth_powered,
                true
            );
        } else {
            owner->bluetooth_tile_->set_state("Unavailable", false, false);
        }

        if (result->night_light_enabled) {
            owner->night_light_tile_->set_state(
                *result->night_light_enabled ? "On" : "Off",
                *result->night_light_enabled,
                true
            );
        } else {
            owner->night_light_tile_->set_state("Unavailable", false, false);
        }

        if (owner->brightness_slider_ != nullptr) {
            owner->brightness_slider_->set_available(
                result->brightness_percent.has_value()
            );
            if (result->brightness_percent) {
                owner->brightness_slider_->set_value(*result->brightness_percent);
            }
        }

        if (owner->volume_slider_ != nullptr) {
            owner->volume_slider_->set_available(result->volume_percent.has_value());
            if (result->volume_percent) {
                owner->volume_slider_->set_value(*result->volume_percent);
            }
        }

        for (std::size_t index = 0;
             index < owner->power_profile_buttons_.size();
             ++index) {
            GtkWidget* button = owner->power_profile_buttons_[index];
            gtk_widget_set_sensitive(button, result->active_profile.has_value());
            gtk_widget_remove_css_class(button, "active");
            gtk_widget_remove_css_class(button, "pending");
            const char* profile = static_cast<const char*>(
                g_object_get_data(G_OBJECT(button), "realmheart-profile")
            );
            if (result->active_profile && profile != nullptr &&
                *result->active_profile == profile) {
                gtk_widget_add_css_class(button, "active");
            }
        }
    }

    state.refresh_in_flight = false;
    if (state.alive.load() && owner != nullptr &&
        state.refresh_pending.exchange(false)) {
        owner->refresh_controls();
    }
    return G_SOURCE_REMOVE;
}

void RightSidebar::destroy_control_refresh_result(gpointer raw) {
    delete static_cast<ControlRefreshResult*>(raw);
}

void RightSidebar::post_control_action(std::function<void()> action) {
    const auto state = async_ui_state_;
    const auto generation = state->generation.fetch_add(1) + 1;
    realmheart::core::shared_task_executor().post([
        state, generation, action = std::move(action)
    ] {
        {
            std::lock_guard lock(state->operation_mutex);
            if (!state->alive.load() || state->generation.load() != generation) return;
            if (action) action();
        }
        g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer raw) -> gboolean {
                auto* payload = static_cast<std::pair<std::shared_ptr<AsyncUiState>, std::uint64_t>*>(raw);
                if (payload->first->alive.load() && payload->first->owner != nullptr &&
                    payload->first->generation.load() == payload->second) {
                    payload->first->owner->refresh_controls();
                }
                return G_SOURCE_REMOVE;
            },
            new std::pair<std::shared_ptr<AsyncUiState>, std::uint64_t>{state, generation},
            +[](gpointer raw) {
                delete static_cast<std::pair<std::shared_ptr<AsyncUiState>, std::uint64_t>*>(raw);
            }
        );
    });
}

void RightSidebar::set_power_profile(const std::string& profile) {
    for (GtkWidget* button : power_profile_buttons_) {
        gtk_widget_remove_css_class(button, "active");
        gtk_widget_remove_css_class(button, "pending");
        const char* candidate = static_cast<const char*>(
            g_object_get_data(G_OBJECT(button), "realmheart-profile")
        );
        if (candidate != nullptr && profile == candidate) {
            gtk_widget_add_css_class(button, "active");
            gtk_widget_add_css_class(button, "pending");
        }
        gtk_widget_set_sensitive(button, FALSE);
    }

    post_control_action([profile] {
        static_cast<void>(services::PowerProfiles::set(profile));
    });
}

} // namespace realmheart::ui::sidebar
