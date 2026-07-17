#include "ui/sidebar/RightSidebar.hpp"

#include "ui/sidebar/SidebarFrame.hpp"

#include "core/TaskExecutor.hpp"
#include "services/Audio.hpp"
#include "services/Bluetooth.hpp"
#include "services/Brightness.hpp"
#include "services/GameMode.hpp"
#include "services/NightLight.hpp"
#include "services/PowerProfiles.hpp"
#include "services/Wifi.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/components/ButtonWidget.hpp"
#include "ui/components/LabelWidget.hpp"
#include "ui/components/NotificationWidget.hpp"
#include "ui/components/SliderWidget.hpp"
#include "ui/components/ToggleWidget.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <utility>

namespace realmheart::ui::sidebar {
namespace {

constexpr double kSidebarHeightFraction = 0.85;
constexpr int kSidebarRightMargin = 2;

} // namespace

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
    // A fixed-height, top-anchored layer surface gives us an exact 85% shell
    // while the computed margin centres it vertically. Keeping bottom
    // unanchored prevents layer-shell from stretching it back to full height.
    layer_spec.anchor_bottom = false;
    layer_spec.margin_top = placement.top_margin;
    layer_spec.margin_right = kSidebarRightMargin;
    apply_layer_surface(GTK_WINDOW(window_), layer_spec);

    setup_layout();
    populate_modules();
}

RightSidebar::~RightSidebar() {
    async_ui_state_->alive = false;
    async_ui_state_->power_profile_label = nullptr;
    modules_.clear();
    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void RightSidebar::setup_layout() {
    frame_ = std::make_unique<SidebarFrame>(
        GTK_WINDOW(window_),
        kDefaultSidebarFrameLayout
    );

    container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(container_, "realmheart-right-sidebar");

    GtkWidget* header = gtk_label_new("System Controls");
    gtk_widget_add_css_class(header, "realmheart-sidebar-header");
    gtk_widget_set_margin_top(header, 12);
    gtk_widget_set_margin_bottom(header, 12);
    gtk_box_append(GTK_BOX(container_), header);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_add_css_class(scroller, "realmheart-sidebar-scroller");
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scroller),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC
    );
    gtk_widget_set_vexpand(scroller, TRUE);

    GtkWidget* module_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_set_data(G_OBJECT(container_), "realmheart-module-box", module_box);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), module_box);
    gtk_box_append(GTK_BOX(container_), scroller);

    frame_->set_child(container_);
    gtk_window_set_child(GTK_WINDOW(window_), frame_->widget());
}

void RightSidebar::add_module(std::unique_ptr<components::BaseWidget> module) {
    if (!module) return;
    GtkWidget* module_box = GTK_WIDGET(
        g_object_get_data(G_OBJECT(container_), "realmheart-module-box")
    );
    gtk_box_append(GTK_BOX(module_box), module->get_widget());
    module->refresh();
    modules_.push_back(std::move(module));
}

void RightSidebar::refresh() {
    for (auto& module : modules_) module->refresh();
}

void RightSidebar::populate_modules() {
    if (const auto wifi = services::Wifi::read()) {
        add_module(std::make_unique<components::LabelWidget>(
            "WiFi",
            wifi->enabled ? "Enabled" : "Disabled",
            [] {
                const auto current = services::Wifi::read();
                if (!current) return std::string("Unavailable");
                if (!current->enabled) return std::string("Disabled");
                return current->ssid.empty()
                    ? std::string("Enabled")
                    : current->ssid;
            }
        ));
    } else {
        add_module(std::make_unique<components::LabelWidget>("WiFi", "Unavailable"));
    }

    if (const auto bluetooth = services::Bluetooth::read()) {
        add_module(std::make_unique<components::ToggleWidget>(
            "Bluetooth",
            bluetooth->powered,
            [](bool powered) {
                return services::Bluetooth::set_powered(powered).success;
            }
        ));
    } else {
        add_module(std::make_unique<components::LabelWidget>("Bluetooth", "Unavailable"));
    }

    add_module(std::make_unique<components::ToggleWidget>(
        "Keep Awake",
        keep_awake_->active(),
        [keep_awake = keep_awake_](bool enabled) {
            return keep_awake->set_enabled(enabled);
        }
    ));

    if (const auto night_light = services::NightLight::read()) {
        add_module(std::make_unique<components::ToggleWidget>(
            "Night Light",
            night_light->enabled,
            [](bool enabled) {
                return services::NightLight::set_enabled(enabled).success;
            }
        ));
    } else {
        add_module(std::make_unique<components::LabelWidget>("Night Light", "Unavailable"));
    }

    if (const auto gamemode = services::GameMode::read()) {
        add_module(std::make_unique<components::ToggleWidget>(
            "Gamemode",
            gamemode->enabled,
            [](bool enabled) {
                return services::GameMode::set_enabled(enabled).success;
            }
        ));
    } else {
        add_module(std::make_unique<components::LabelWidget>("Gamemode", "Unavailable"));
    }

    auto power_profile_label = std::make_unique<components::LabelWidget>(
        "Power Profile",
        services::PowerProfiles::current().value_or("Unavailable"),
        [] { return services::PowerProfiles::current().value_or("Unavailable"); }
    );
    async_ui_state_->power_profile_label = power_profile_label.get();
    add_module(std::move(power_profile_label));

    const auto async_ui_state = async_ui_state_;
    add_module(std::make_unique<components::ButtonWidget>("Power Profile", [async_ui_state] {
        const auto generation = async_ui_state->power_profile_generation.fetch_add(1) + 1;
        realmheart::core::shared_task_executor().post([async_ui_state, generation] {
            std::optional<std::string> next;
            {
                std::lock_guard mutation_lock(async_ui_state->power_profile_mutex);
                if (!async_ui_state->alive.load() ||
                    async_ui_state->power_profile_generation.load() != generation) {
                    return;
                }
                next = services::PowerProfiles::cycle();
            }
            g_idle_add_full(
                G_PRIORITY_DEFAULT_IDLE,
                +[](gpointer raw) -> gboolean {
                    auto* payload = static_cast<std::pair<std::shared_ptr<AsyncUiState>, std::optional<std::string>>*>(raw);
                    if (payload->first->alive.load() && payload->first->power_profile_label != nullptr) {
                        if (payload->second) {
                            payload->first->power_profile_label->set_value(*payload->second);
                        } else {
                            payload->first->power_profile_label->refresh();
                        }
                    }
                    return G_SOURCE_REMOVE;
                },
                new std::pair<std::shared_ptr<AsyncUiState>, std::optional<std::string>>{async_ui_state, std::move(next)},
                +[](gpointer raw) {
                    delete static_cast<std::pair<std::shared_ptr<AsyncUiState>, std::optional<std::string>>*>(raw);
                }
            );
        });
    }));

    if (const auto brightness = services::Brightness::read()) {
        add_module(std::make_unique<components::SliderWidget>(
            "Brightness",
            0,
            100,
            brightness->percent,
            [](double value) {
                const auto mutation = services::Brightness::set_percent(
                    static_cast<int>(std::lround(value))
                );
                if (!mutation.success) return std::optional<double>{};
                return std::optional<double>{mutation.state.percent};
            },
            show_brightness_osd_
        ));
    }

    if (const auto audio = services::Audio::read_default_sink()) {
        add_module(std::make_unique<components::SliderWidget>(
            "Volume",
            0,
            150,
            audio->volume * 100.0,
            [](double value) {
                const auto mutation = services::Audio::set_default_sink_volume(value / 100.0);
                if (!mutation.success) return std::optional<double>{};
                return std::optional<double>{mutation.state.volume * 100.0};
            },
            show_volume_osd_
        ));
    }

    add_module(std::make_unique<components::NotificationWidget>(notification_history_));
}

} // namespace realmheart::ui::sidebar
