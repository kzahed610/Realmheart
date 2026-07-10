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

#include <gtk/gtk.h>
#include <cmath>
#include <iostream>
#include <optional>
#include <vector>

namespace realmheart::ui::sidebar {

// --- Concrete Modules ---

class LabelModule : public SidebarModule {
public:
    LabelModule(const std::string& label, const std::string& value) : SidebarModule(label) {
        box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(box_, 12);
        gtk_widget_set_margin_end(box_, 12);
        gtk_widget_set_margin_top(box_, 6);
        gtk_widget_set_margin_bottom(box_, 6);

        GtkWidget* lbl_name = gtk_label_new(label.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
        gtk_box_append(GTK_BOX(box_), lbl_name);

        val_label_ = gtk_label_new(value.c_str());
        gtk_label_set_xalign(GTK_LABEL(val_label_), 1.0);
        gtk_box_append(GTK_BOX(box_), val_label_);
    }

    GtkWidget* get_widget() override { return box_; }

    void set_value(const std::string& value) {
        gtk_label_set_text(GTK_LABEL(val_label_), value.c_str());
    }

private:
    GtkWidget* box_;
    GtkWidget* val_label_;
};

class NotificationListModule : public SidebarModule {
public:
    NotificationListModule(services::NotificationHistory& history) 
        : SidebarModule("Notifications"), history_(history) {
        
        box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start(box_, 12);
        gtk_widget_set_margin_end(box_, 12);
        gtk_widget_set_margin_top(box_, 6);
        gtk_widget_set_margin_bottom(box_, 6);

        refresh();
    }

    GtkWidget* get_widget() override { return box_; }

    void refresh() override {
        // Clear existing entries
        GtkWidget* child = gtk_widget_get_first_child(box_);
        while (child) {
            GtkWidget* next = gtk_widget_get_next_sibling(child);
            gtk_box_remove(GTK_BOX(box_), child);
            child = next;
        }

        auto snapshot = history_.snapshot();
        if (snapshot.entries.empty()) {
            GtkWidget* empty_lbl = gtk_label_new("No notifications");
            gtk_widget_set_margin_top(empty_lbl, 10);
            gtk_box_append(GTK_BOX(box_), empty_lbl);
            return;
        }

        for (const auto& entry : snapshot.entries) {
            GtkWidget* row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_margin_bottom(row, 8);

            GtkWidget* summary = gtk_label_new(entry.summary.c_str());
            gtk_label_set_xalign(GTK_LABEL(summary), 0.0);
            // Make unread bold or different
            if (entry.unread) {
                gtk_label_set_markup(GTK_LABEL(summary), ("<b>" + entry.summary + "</b>").c_str());
            }
            gtk_box_append(GTK_BOX(row), summary);

            GtkWidget* body = gtk_label_new(entry.body.c_str());
            gtk_label_set_xalign(GTK_LABEL(body), 0.0);
            gtk_widget_set_margin_start(body, 10);
            gtk_box_append(GTK_BOX(row), body);

            gtk_box_append(GTK_BOX(box_), row);
        }
    }

private:
    GtkWidget* box_;
    services::NotificationHistory& history_;
};

class ToggleModule : public SidebarModule {
public:
    ToggleModule(const std::string& label, bool initial, std::function<bool(bool)> on_toggle)
        : SidebarModule(label), on_toggle_(on_toggle) {
        box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(box_, 12);
        gtk_widget_set_margin_end(box_, 12);
        gtk_widget_set_margin_top(box_, 6);
        gtk_widget_set_margin_bottom(box_, 6);

        GtkWidget* lbl_name = gtk_label_new(label.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
        gtk_box_append(GTK_BOX(box_), lbl_name);

        switch_ = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(switch_), initial);
        g_signal_connect(switch_, "state-set", G_CALLBACK(+[](GtkSwitch*, gboolean state, gpointer data) -> gboolean {
            auto* self = static_cast<ToggleModule*>(data);
            if (self->on_toggle_(state)) return FALSE;
            return TRUE;
        }), this);
        gtk_box_append(GTK_BOX(box_), switch_);
    }

    GtkWidget* get_widget() override { return box_; }

private:
    GtkWidget* box_;
    GtkWidget* switch_;
    std::function<bool(bool)> on_toggle_;
};

class SliderModule : public SidebarModule {
public:
    SliderModule(
        const std::string& label,
        double min,
        double max,
        double initial,
        std::function<std::optional<double>(double)> on_change
    ) : SidebarModule(label), confirmed_value_(initial), pending_value_(initial), on_change_(on_change) {
        box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(box_, 12);
        gtk_widget_set_margin_end(box_, 12);
        gtk_widget_set_margin_top(box_, 6);
        gtk_widget_set_margin_bottom(box_, 6);

        GtkWidget* lbl_name = gtk_label_new(label.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
        gtk_box_append(GTK_BOX(box_), lbl_name);

        scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1.0);
        gtk_range_set_value(GTK_RANGE(scale_), initial);
        g_signal_connect(scale_, "value-changed", G_CALLBACK(+[](GtkRange* r, gpointer data) {
            auto* self = static_cast<SliderModule*>(data);
            if (self->updating_) return;
            self->pending_value_ = gtk_range_get_value(r);
            if (self->debounce_source_ != 0) g_source_remove(self->debounce_source_);
            self->debounce_source_ = g_timeout_add(120, +[](gpointer callback_data) -> gboolean {
                auto* module = static_cast<SliderModule*>(callback_data);
                module->debounce_source_ = 0;
                const auto actual = module->on_change_(module->pending_value_);
                if (actual) module->confirmed_value_ = *actual;

                module->updating_ = true;
                gtk_range_set_value(GTK_RANGE(module->scale_), module->confirmed_value_);
                module->updating_ = false;
                return G_SOURCE_REMOVE;
            }, self);
        }), this);
        gtk_box_append(GTK_BOX(box_), scale_);
    }

    ~SliderModule() override {
        if (debounce_source_ != 0) g_source_remove(debounce_source_);
    }

    GtkWidget* get_widget() override { return box_; }

private:
    GtkWidget* box_;
    GtkWidget* scale_;
    guint debounce_source_ = 0;
    bool updating_ = false;
    double confirmed_value_ = 0.0;
    double pending_value_ = 0.0;
    std::function<std::optional<double>(double)> on_change_;
};

class ButtonModule : public SidebarModule {
public:
    ButtonModule(const std::string& label, std::function<void()> on_click) 
        : SidebarModule(label), on_click_(on_click) {
        box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(box_, 12);
        gtk_widget_set_margin_end(box_, 12);
        gtk_widget_set_margin_top(box_, 6);
        gtk_widget_set_margin_bottom(box_, 6);

        GtkWidget* lbl_name = gtk_label_new(label.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl_name), 0.0);
        gtk_box_append(GTK_BOX(box_), lbl_name);

        btn_ = gtk_button_new_with_label("Cycle");
        g_signal_connect(btn_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<ButtonModule*>(data);
            self->on_click_();
            return FALSE;
        }), this);
        gtk_box_append(GTK_BOX(box_), btn_);
    }

    GtkWidget* get_widget() override { return box_; }

private:
    GtkWidget* box_;
    GtkWidget* btn_;
    std::function<void()> on_click_;
};

// --- RightSidebar Implementation ---

RightSidebar::RightSidebar(
    GtkApplication* app,
    services::NotificationHistory& notification_history
) : app_(app), notification_history_(notification_history) {
    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Right Sidebar");
    gtk_window_set_default_size(GTK_WINDOW(window_), 300, 800);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    
    apply_layer_surface(GTK_WINDOW(window_), make_layer_surface_spec("realmheart-sidebar", LayerSurfaceLevel::Top, LayerKeyboardMode::Exclusive));
    
    setup_layout();
    populate_modules();
}

void RightSidebar::setup_layout() {
    container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(container_, 300, -1);
    
    GtkWidget* header = gtk_label_new("System Controls");
    gtk_widget_set_margin_top(header, 12);
    gtk_widget_set_margin_bottom(header, 12);
    gtk_box_append(GTK_BOX(container_), header);

    gtk_window_set_child(GTK_WINDOW(window_), container_);
}

void RightSidebar::add_module(std::unique_ptr<SidebarModule> module) {
    modules_.push_back(std::move(module));
    gtk_box_append(GTK_BOX(container_), modules_.back()->get_widget());
}

void RightSidebar::refresh() {
    for (auto& module : modules_) module->refresh();
}

void RightSidebar::populate_modules() {
    if (const auto wifi = services::Wifi::read()) {
        add_module(std::make_unique<ToggleModule>("WiFi", wifi->enabled, [](bool enabled) {
            return services::Wifi::set_enabled(enabled).success;
        }));
    } else {
        add_module(std::make_unique<LabelModule>("WiFi", "Unavailable"));
    }

    if (const auto bluetooth = services::Bluetooth::read()) {
        add_module(std::make_unique<ToggleModule>("Bluetooth", bluetooth->powered, [](bool powered) {
            return services::Bluetooth::set_powered(powered).success;
        }));
    } else {
        add_module(std::make_unique<LabelModule>("Bluetooth", "Unavailable"));
    }

    add_module(std::make_unique<ToggleModule>("Keep Awake", keep_awake_.active(), [this](bool enabled) {
        return keep_awake_.set_enabled(enabled);
    }));

    if (const auto night_light = services::NightLight::read()) {
        add_module(std::make_unique<ToggleModule>("Night Light", night_light->enabled, [](bool enabled) {
            return services::NightLight::set_enabled(enabled).success;
        }));
    } else {
        add_module(std::make_unique<LabelModule>("Night Light", "Unavailable"));
    }

    if (const auto gamemode = services::GameMode::read()) {
        add_module(std::make_unique<ToggleModule>("Gamemode", gamemode->enabled, [](bool enabled) {
            return services::GameMode::set_enabled(enabled).success;
        }));
    } else {
        add_module(std::make_unique<LabelModule>("Gamemode", "Unavailable"));
    }

    const auto profile = services::PowerProfiles::current();
    add_module(std::make_unique<LabelModule>("Power Profile", profile.value_or("Unavailable")));
    add_module(std::make_unique<ButtonModule>("Power Profile", []() {
        if (auto next = services::PowerProfiles::cycle()) {
            std::cout << "Power profile cycled to: " << *next << "\n";
        }
    }));

    if (auto b = services::Brightness::read()) {
        add_module(std::make_unique<SliderModule>("Brightness", 0, 100, b->percent, [](double value) {
            static_cast<void>(services::Brightness::set(static_cast<int>(std::lround(value))));
            realmheart::core::send_shell_command(realmheart::core::ShellCommand::ShowOSDBrightness);
            const auto readback = services::Brightness::read();
            if (!readback) return std::optional<double>{};
            return std::optional<double>{readback->percent};
        }));
    }

    if (const auto audio = services::Audio::read_default_sink()) {
        add_module(std::make_unique<SliderModule>("Volume", 0, 150, audio->volume * 100.0, [](double value) {
            static_cast<void>(services::Audio::set_default_sink_volume(value / 100.0));
            realmheart::core::send_shell_command(realmheart::core::ShellCommand::ShowOSDVolume);
            const auto readback = services::Audio::read_default_sink();
            if (!readback) return std::optional<double>{};
            return std::optional<double>{readback->volume * 100.0};
        }));
    }

    add_module(std::make_unique<NotificationListModule>(notification_history_));
}

} // namespace realmheart::ui::sidebar
