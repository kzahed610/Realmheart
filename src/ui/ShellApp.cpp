#include "ui/ShellApp.hpp"

#include "core/ShellControl.hpp"
#include "services/NotificationDaemon.hpp"
#include "services/NotificationServer.hpp"
#include "services/Notifications.hpp"
#include "ui/ShellState.hpp"
#include "ui/bar/VerticalBar.hpp"
#include "ui/sidebar/RightSidebar.hpp"
#include "services/Audio.hpp"
#include "services/Brightness.hpp"
#include "ui/NotificationToast.hpp"
#include "ui/OSDOverlay.hpp"

#include <gtk/gtk.h>

#include <iostream>
#include <memory>

namespace realmheart::ui {
namespace {

class ShellRuntime {
public:
    explicit ShellRuntime(GtkApplication* application)
        : application_(application),
          notification_server_(notification_history_),
          notification_daemon_(notification_server_, notification_history_) {

        toast_ = std::make_unique<NotificationToast>(application_);
        osd_ = std::make_unique<OSDOverlay>(application_);

        notification_server_.set_notification_handler([this](const auto& entry) {
            toast_->show(entry, 5000);
        });

        if (!notification_daemon_.start()) {
            std::cerr << "Unable to start Realmheart notification daemon\n";
        }
    }

    ~ShellRuntime() {
        if (sidebar_) {
            gtk_window_destroy(GTK_WINDOW(sidebar_->get_window()));
            sidebar_.reset();
        }
        if (bar_ != nullptr) {
            gtk_window_destroy(bar_);
            bar_ = nullptr;
        }
    }

    void activate() {
        ensure_initialized();
        state_.show_bar();
        apply_bar_visibility();
    }

    void toggle_right_sidebar() {
        ensure_initialized();
        state_.toggle_right_sidebar();
        apply_right_sidebar_visibility();
    }

    void show_osd_volume() {
        if (auto audio = services::Audio::read_default_sink()) {
            osd_->show_volume(audio->volume);
        }
    }

    void show_osd_brightness() {
        if (auto bri = services::Brightness::read()) {
            osd_->show_brightness(bri->percent);
        }
    }

    void toggle_bar() {
        ensure_initialized();
        state_.toggle_bar();
        apply_bar_visibility();
    }

    void quit() {
        g_application_quit(G_APPLICATION(application_));
    }

private:
    void ensure_initialized() {
        if (bar_ == nullptr) bar_ = bar::present_vertical_bar(application_);
        if (!sidebar_) {
            sidebar_ = std::make_unique<sidebar::RightSidebar>(application_, notification_history_);
            gtk_widget_set_visible(sidebar_->get_window(), FALSE);
        }
    }

    void apply_right_sidebar_visibility() {
        GtkWidget* window = sidebar_->get_window();
        if (state_.right_sidebar_visible()) {
            sidebar_->refresh();
            gtk_window_present(GTK_WINDOW(window));
        } else {
            gtk_widget_set_visible(window, FALSE);
        }
    }

    void apply_bar_visibility() {
        GtkWidget* window = GTK_WIDGET(bar_);
        if (state_.bar_visible()) {
            gtk_window_present(bar_);
        } else {
            gtk_widget_set_visible(window, FALSE);
        }
    }

    GtkApplication* application_ = nullptr;
    services::NotificationHistory notification_history_;
    services::NotificationServer notification_server_;
    services::NotificationDaemon notification_daemon_;
    std::unique_ptr<NotificationToast> toast_;
    std::unique_ptr<OSDOverlay> osd_;
    GtkWindow* bar_ = nullptr;
    std::unique_ptr<sidebar::RightSidebar> sidebar_;
    ShellState state_;
};

void activate_shell(GtkApplication*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->activate();
}

void toggle_right_sidebar_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->toggle_right_sidebar();
}

void show_osd_volume_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->show_osd_volume();
}

void show_osd_brightness_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->show_osd_brightness();
}

void toggle_bar_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->toggle_bar();
}

void quit_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->quit();
}

constexpr GActionEntry kShellActions[] = {
    {"sidebar-right-toggle", toggle_right_sidebar_action, nullptr, nullptr, nullptr, {}},
    {"bar-toggle", toggle_bar_action, nullptr, nullptr, nullptr, {}},
    {"osd-volume", show_osd_volume_action, nullptr, nullptr, nullptr, {}},
    {"osd-brightness", show_osd_brightness_action, nullptr, nullptr, nullptr, {}},
    {"quit", quit_action, nullptr, nullptr, nullptr, {}},
};

} // namespace

int run_shell() {
    GtkApplication* application = gtk_application_new(
        realmheart::core::shell_application_id().data(),
        G_APPLICATION_DEFAULT_FLAGS
    );

    int status = 1;
    {
        ShellRuntime runtime(application);
        g_action_map_add_action_entries(
            G_ACTION_MAP(application),
            kShellActions,
            static_cast<gint>(sizeof(kShellActions) / sizeof(kShellActions[0])),
            &runtime
        );
        g_signal_connect(application, "activate", G_CALLBACK(activate_shell), &runtime);
        status = g_application_run(G_APPLICATION(application), 0, nullptr);
    }

    g_object_unref(application);
    return status;
}

} // namespace realmheart::ui
