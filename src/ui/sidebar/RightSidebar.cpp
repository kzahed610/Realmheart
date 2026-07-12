#include "ui/components/BaseWidget.hpp"
#include "ui/sidebar/RightSidebar.hpp"

#include "ui/LayerSurface.hpp"
#include "core/ShellCommand.hpp"
#include "core/ShellControl.hpp"
#include "services/Audio.hpp"
#include "services/Bluetooth.hpp"
#include "services/Brightness.hpp"
#include "services/GameMode.hpp"
#include "services/NightLight.hpp"
#include "services/PowerProfiles.hpp"
#include "services/Notifications.hpp"
#include "services/Wifi.hpp"
#include "services/ThemeService.hpp"
#include "ui/components/LabelWidget.hpp"
#include "ui/components/ToggleWidget.hpp"
#include "ui/components/SliderWidget.hpp"
#include "ui/components/ButtonWidget.hpp"
#include "ui/components/NotificationWidget.hpp"

#include <gtk/gtk.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace realmheart::ui::sidebar {

RightSidebar::RightSidebar(
    GtkApplication* app,
    services::NotificationHistory& notification_history,
    std::shared_ptr<services::ThemeService> theme_service
) : app_(app), notification_history_(notification_history), theme_service_(std::move(theme_service)) {
    keep_awake_ = std::make_shared<services::KeepAwake>();
    
    // Subscribe to shared theme changes
    theme_service_->subscribe([this](const services::Palette& palette) {
        for (auto& module : modules_) {
            if (auto themeable = std::dynamic_pointer_cast<components::ThemeableWidget>(module)) {
                themeable->apply_theme(palette);
            }
        }
    });

    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Right Sidebar");
    gtk_window_set_default_size(GTK_WINDOW(window_), 300, 800);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    
    apply_layer_surface(GTK_WINDOW(window_), make_layer_surface_spec("realmheart-right-sidebar", LayerSurfaceLevel::Overlay, LayerKeyboardMode::OnDemand));
    
    setup_layout();
    populate_modules();
    
    // Apply initial theme
    const auto& initial_palette = theme_service_->get_palette();
    for (auto& module : modules_) {
        if (auto themeable = std::dynamic_pointer_cast<components::ThemeableWidget>(module)) {
            themeable->apply_theme(initial_palette);
        }
    }
}

void RightSidebar::setup_layout() {
    container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(container_, "realmheart-right-sidebar");
    gtk_widget_set_size_request(container_, 300, -1);
    
    GtkWidget* header = gtk_label_new("System Controls");
    gtk_widget_set_margin_top(header, 12);
    gtk_widget_set_margin_bottom(header, 12);
    gtk_box_append(GTK_BOX(container_), header);

    gtk_window_set_child(GTK_WINDOW(window_), container_);
}

void RightSidebar::add_module(std::shared_ptr<components::BaseWidget> module) {
    modules_.push_back(std::static_pointer_cast<SidebarModule>(module));
    gtk_box_append(GTK_BOX(container_), module->get_widget());
    module->refresh();
    
    // Apply theme to new module immediately
    if (auto themeable = std::dynamic_pointer_cast<components::ThemeableWidget>(module)) {
        themeable->apply_theme(theme_service_->get_palette());
    }
}

void RightSidebar::refresh() {
    for (auto& module : modules_) module->refresh();
}

void RightSidebar::populate_modules() {
    if (const auto wifi = services::Wifi::read()) {
        add_module(std::make_shared<components::LabelWidget>("WiFi", wifi->enabled ? "Enabled" : "Disabled", []() -> std::string {
            return services::Wifi::read() ? (services::Wifi::read()->enabled ? "Enabled" : "Disabled") : "Unavailable";
        }));
    } else {
        add_module(std::make_shared<components::LabelWidget>("WiFi", "Unavailable"));
    }

    if (const auto bluetooth = services::Bluetooth::read()) {
        add_module(std::make_shared<components::ToggleWidget>("Bluetooth", bluetooth->powered, [](bool powered) {
            return services::Bluetooth::set_powered(powered).success;
        }));
    } else {
        add_module(std::make_shared<components::LabelWidget>("Bluetooth", "Unavailable"));
    }

    add_module(std::make_shared<components::ToggleWidget>("Keep Awake", keep_awake_->active(), [ka = keep_awake_](bool enabled) {
        return ka->set_enabled(enabled);
    }));

    if (const auto night_light = services::NightLight::read()) {
        add_module(std::make_shared<components::ToggleWidget>("Night Light", night_light->enabled, [](bool enabled) {
            return services::NightLight::set_enabled(enabled).success;
        }));
        add_module(std::make_shared<components::LabelWidget>("Night Light", "Unavailable"));
    } else {
        add_module(std::make_shared<components::LabelWidget>("Night Light", "Unavailable"));
    }

    if (const auto gamemode = services::GameMode::read()) {
        add_module(std::make_shared<components::ToggleWidget>("Gamemode", gamemode->enabled, [](bool enabled) {
            return services::GameMode::set_enabled(enabled).success;
        }));
    } else {
        add_module(std::make_shared<components::LabelWidget>("Gamemode", "Unavailable"));
    }

    const auto profile = services::PowerProfiles::current();
    add_module(std::make_shared<components::LabelWidget>(
        "Power Profile",
        profile.value_or("Unavailable"),
        [] { return services::PowerProfiles::current().value_or("Unavailable"); }
    ));
    add_module(std::make_shared<components::ButtonWidget>("Power Profile", []() {
        std::thread([]() {
            if (auto next = services::PowerProfiles::cycle()) {
                std::cout << "Power profile cycled to: " << *next << "\n";
            }
        }).detach();
    }));

    if (auto b = services::Brightness::read()) {
        add_module(std::make_shared<components::SliderWidget>("Brightness", 0, 100, b->percent, [](double value) {
            static_cast<void>(services::Brightness::set(static_cast<int>(std::lround(value))));
            realmheart::core::send_shell_command(realmheart::core::ShellCommand::ShowOSDBrightness);
            const auto readback = services::Brightness::read();
            if (!readback) return std::optional<double>{};
            return std::optional<double>{readback->percent};
        }));
    }

    if (const auto audio = services::Audio::read_default_sink()) {
        add_module(std::make_shared<components::SliderWidget>("Volume", 0, 150, audio->volume * 100.0, [](double value) {
            static_cast<void>(services::Audio::set_default_sink_volume(value / 100.0));
            realmheart::core::send_shell_command(realmheart::core::ShellCommand::ShowOSDVolume);
            const auto readback = services::Audio::read_default_sink();
            if (!readback) return std::optional<double>{};
            return std::optional<double>{readback->volume * 100.0};
        }));
    }

    add_module(std::make_shared<components::NotificationWidget>(notification_history_));
}

} // namespace realmheart::ui::sidebar
