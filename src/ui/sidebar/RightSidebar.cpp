#include "ui/sidebar/RightSidebar.hpp"

#include "core/ShellCommand.hpp"
#include "core/ShellControl.hpp"
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

#include <cmath>
#include <iostream>
#include <optional>
#include <thread>
#include <utility>

namespace realmheart::ui::sidebar {

RightSidebar::RightSidebar(
    GtkApplication* app,
    services::NotificationHistory& notification_history
) : app_(app),
    keep_awake_(std::make_shared<services::KeepAwake>()),
    notification_history_(notification_history) {
    window_ = gtk_application_window_new(app_);
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Right Sidebar");
    gtk_window_set_default_size(GTK_WINDOW(window_), 300, 800);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);

    apply_layer_surface(
        GTK_WINDOW(window_),
        make_layer_surface_spec(
            "realmheart-right-sidebar",
            LayerSurfaceLevel::Overlay,
            LayerKeyboardMode::OnDemand
        )
    );

    setup_layout();
    populate_modules();
}

RightSidebar::~RightSidebar() {
    modules_.clear();
    if (window_ != nullptr) {
        gtk_window_destroy(GTK_WINDOW(window_));
        window_ = nullptr;
    }
}

void RightSidebar::setup_layout() {
    container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(container_, "realmheart-right-sidebar");
    gtk_widget_set_size_request(container_, 300, -1);

    GtkWidget* header = gtk_label_new("System Controls");
    gtk_widget_add_css_class(header, "realmheart-sidebar-header");
    gtk_widget_set_margin_top(header, 12);
    gtk_widget_set_margin_bottom(header, 12);
    gtk_box_append(GTK_BOX(container_), header);

    GtkWidget* scroller = gtk_scrolled_window_new();
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

    gtk_window_set_child(GTK_WINDOW(window_), container_);
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

    add_module(std::make_unique<components::LabelWidget>(
        "Power Profile",
        services::PowerProfiles::current().value_or("Unavailable"),
        [] { return services::PowerProfiles::current().value_or("Unavailable"); }
    ));
    add_module(std::make_unique<components::ButtonWidget>("Power Profile", [] {
        std::thread([] {
            if (const auto next = services::PowerProfiles::cycle()) {
                std::cout << "Power profile cycled to: " << *next << '\n';
            }
        }).detach();
    }));

    if (const auto brightness = services::Brightness::read()) {
        add_module(std::make_unique<components::SliderWidget>(
            "Brightness",
            0,
            100,
            brightness->percent,
            [](double value) {
                static_cast<void>(
                    services::Brightness::set(static_cast<int>(std::lround(value)))
                );
                realmheart::core::send_shell_command(
                    realmheart::core::ShellCommand::ShowOSDBrightness
                );
                const auto readback = services::Brightness::read();
                if (!readback) return std::optional<double>{};
                return std::optional<double>{readback->percent};
            }
        ));
    }

    if (const auto audio = services::Audio::read_default_sink()) {
        add_module(std::make_unique<components::SliderWidget>(
            "Volume",
            0,
            150,
            audio->volume * 100.0,
            [](double value) {
                static_cast<void>(
                    services::Audio::set_default_sink_volume(value / 100.0)
                );
                realmheart::core::send_shell_command(
                    realmheart::core::ShellCommand::ShowOSDVolume
                );
                const auto readback = services::Audio::read_default_sink();
                if (!readback) return std::optional<double>{};
                return std::optional<double>{readback->volume * 100.0};
            }
        ));
    }

    add_module(std::make_unique<components::NotificationWidget>(notification_history_));
}

} // namespace realmheart::ui::sidebar
