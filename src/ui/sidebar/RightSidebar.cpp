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

// --- Concrete Modules ---

class LabelModule : public SidebarModule, public std::enable_shared_from_this<LabelModule> {
public:
    using Reader = std::function<std::string()>;

    LabelModule(const std::string& label, const std::string& value, Reader reader = {})
        : SidebarModule(label), reader_(std::move(reader)) {
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

    void refresh() override {
        if (!reader_ || refresh_in_flight_.exchange(true)) return;

        const auto weak = weak_from_this();
        const auto reader = reader_;
        std::thread([weak, reader] {
            std::string value;
            try {
                value = reader();
            } catch (const std::exception&) {
                value = "Unavailable";
            }

            struct Result {
                std::weak_ptr<LabelModule> module;
                std::string value;
            };
            g_idle_add(+[](gpointer data) -> gboolean {
                std::unique_ptr<Result> result(static_cast<Result*>(data));
                if (auto module = result->module.lock()) {
                    module->set_value(result->value);
                    module->refresh_in_flight_ = false;
                }
                return G_SOURCE_REMOVE;
            }, new Result{weak, std::move(value)});
        }).detach();
    }

    void set_value(const std::string& value) {
        gtk_label_set_text(GTK_LABEL(val_label_), value.c_str());
    }

private:
    GtkWidget* box_;
    GtkWidget* val_label_;
    Reader reader_;
    std::atomic_bool refresh_in_flight_ = false;
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
                gchar* markup = g_markup_printf_escaped("<b>%s</b>", entry.summary.c_str());
                gtk_label_set_markup(GTK_LABEL(summary), markup);
                g_free(markup);
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

class ToggleModule : public SidebarModule, public std::enable_shared_from_this<ToggleModule> {
public:
    ToggleModule(const std::string& label, bool initial, std::function<bool(bool)> on_toggle)
        : SidebarModule(label), worker_state_(std::make_shared<WorkerState>()) {
        worker_state_->on_toggle = std::move(on_toggle);

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
            if (self->updating_) return FALSE;

            {
                std::lock_guard lock(self->worker_state_->mutex);
                self->worker_state_->target_state = state;
                self->worker_state_->has_pending = true;
            }
            self->worker_state_->cv.notify_one();
            return FALSE;
        }), this);
        gtk_box_append(GTK_BOX(box_), switch_);
    }

    void init() override {
        if (worker_.joinable()) return;

        const auto state = worker_state_;
        const auto weak = weak_from_this();
        worker_ = std::thread([state, weak] {
            while (true) {
                bool state_to_set = false;
                {
                    std::unique_lock lock(state->mutex);
                    state->cv.wait(lock, [&state] { return state->shutdown || state->has_pending; });
                    if (state->shutdown) return;
                    state_to_set = state->target_state;
                    state->has_pending = false;
                }

                bool succeeded = false;
                try {
                    succeeded = state->on_toggle(state_to_set);
                } catch (const std::exception&) {
                    succeeded = false;
                }

                if (!succeeded) {
                    struct AsyncState {
                        std::weak_ptr<ToggleModule> module;
                        bool requested_state;
                    };
                    g_idle_add(+[](gpointer data) -> gboolean {
                        std::unique_ptr<AsyncState> result(static_cast<AsyncState*>(data));
                        if (auto module = result->module.lock()) {
                            module->set_active(!result->requested_state);
                        }
                        return G_SOURCE_REMOVE;
                    }, new AsyncState{weak, state_to_set});
                }
            }
        });
    }

    ~ToggleModule() override {
        {
            std::lock_guard lock(worker_state_->mutex);
            worker_state_->shutdown = true;
        }
        worker_state_->cv.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    GtkWidget* get_widget() override { return box_; }

    void set_active(bool active) {
        updating_ = true;
        gtk_switch_set_active(GTK_SWITCH(switch_), active);
        updating_ = false;
    }

private:
    struct WorkerState {
        std::mutex mutex;
        std::condition_variable cv;
        std::function<bool(bool)> on_toggle;
        bool shutdown = false;
        bool has_pending = false;
        bool target_state = false;
    };

    GtkWidget* box_;
    GtkWidget* switch_;
    bool updating_ = false;
    std::shared_ptr<WorkerState> worker_state_;
    std::thread worker_;
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
    keep_awake_ = std::make_shared<services::KeepAwake>();
    window_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window_), "Realmheart Right Sidebar");
    gtk_window_set_default_size(GTK_WINDOW(window_), 300, 800);
    gtk_window_set_resizable(GTK_WINDOW(window_), FALSE);
    gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
    
    apply_layer_surface(GTK_WINDOW(window_), make_layer_surface_spec("realmheart-right-sidebar", LayerSurfaceLevel::Overlay, LayerKeyboardMode::OnDemand));
    
    setup_layout();
    populate_modules();
}

void RightSidebar::setup_layout() {
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        ".realmheart-right-sidebar { background: alpha(#11111b, 0.97); color: #cdd6f4; padding: 14px; border-left: 1px solid #cba6f7; }"
        ".realmheart-right-sidebar button { min-height: 34px; }"
    );
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    container_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(container_, "realmheart-right-sidebar");
    gtk_widget_set_size_request(container_, 300, -1);
    
    GtkWidget* header = gtk_label_new("System Controls");
    gtk_widget_set_margin_top(header, 12);
    gtk_widget_set_margin_bottom(header, 12);
    gtk_box_append(GTK_BOX(container_), header);

    gtk_window_set_child(GTK_WINDOW(window_), container_);
}

void RightSidebar::add_module(std::shared_ptr<SidebarModule> module) {
    modules_.push_back(std::move(module));
    gtk_box_append(GTK_BOX(container_), modules_.back()->get_widget());
    modules_.back()->init();
}

void RightSidebar::refresh() {
    for (auto& module : modules_) module->refresh();
}

void RightSidebar::populate_modules() {
    if (const auto wifi = services::Wifi::read()) {
        add_module(std::make_shared<ToggleModule>("WiFi", wifi->enabled, [](bool enabled) {
            return services::Wifi::set_enabled(enabled).success;
        }));
    } else {
        add_module(std::make_shared<LabelModule>("WiFi", "Unavailable"));
    }

    if (const auto bluetooth = services::Bluetooth::read()) {
        add_module(std::make_shared<ToggleModule>("Bluetooth", bluetooth->powered, [](bool powered) {
            return services::Bluetooth::set_powered(powered).success;
        }));
    } else {
        add_module(std::make_shared<LabelModule>("Bluetooth", "Unavailable"));
    }

    add_module(std::make_shared<ToggleModule>("Keep Awake", keep_awake_->active(), [ka = keep_awake_](bool enabled) {
        return ka->set_enabled(enabled);
    }));

    if (const auto night_light = services::NightLight::read()) {
        add_module(std::make_shared<ToggleModule>("Night Light", night_light->enabled, [](bool enabled) {
            return services::NightLight::set_enabled(enabled).success;
        }));
    } else {
        add_module(std::make_shared<LabelModule>("Night Light", "Unavailable"));
    }

    if (const auto gamemode = services::GameMode::read()) {
        add_module(std::make_shared<ToggleModule>("Gamemode", gamemode->enabled, [](bool enabled) {
            return services::GameMode::set_enabled(enabled).success;
        }));
    } else {
        add_module(std::make_shared<LabelModule>("Gamemode", "Unavailable"));
    }

    const auto profile = services::PowerProfiles::current();
    add_module(std::make_shared<LabelModule>(
        "Power Profile",
        profile.value_or("Unavailable"),
        [] { return services::PowerProfiles::current().value_or("Unavailable"); }
    ));
    add_module(std::make_shared<ButtonModule>("Power Profile", []() {
        std::thread([]() {
            if (auto next = services::PowerProfiles::cycle()) {
                std::cout << "Power profile cycled to: " << *next << "\n";
            }
        }).detach();
    }));

    if (auto b = services::Brightness::read()) {
        add_module(std::make_shared<SliderModule>("Brightness", 0, 100, b->percent, [](double value) {
            static_cast<void>(services::Brightness::set(static_cast<int>(std::lround(value))));
            realmheart::core::send_shell_command(realmheart::core::ShellCommand::ShowOSDBrightness);
            const auto readback = services::Brightness::read();
            if (!readback) return std::optional<double>{};
            return std::optional<double>{readback->percent};
        }));
    }

    if (const auto audio = services::Audio::read_default_sink()) {
        add_module(std::make_shared<SliderModule>("Volume", 0, 150, audio->volume * 100.0, [](double value) {
            static_cast<void>(services::Audio::set_default_sink_volume(value / 100.0));
            realmheart::core::send_shell_command(realmheart::core::ShellCommand::ShowOSDVolume);
            const auto readback = services::Audio::read_default_sink();
            if (!readback) return std::optional<double>{};
            return std::optional<double>{readback->volume * 100.0};
        }));
    }

    add_module(std::make_shared<NotificationListModule>(notification_history_));
}

} // namespace realmheart::ui::sidebar
