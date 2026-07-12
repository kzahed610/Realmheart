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
#include "services/UtilityManager.hpp"
#include "services/SessionManager.hpp"
#include "services/BatteryService.hpp"
#include "services/MediaService.hpp"
#include "ui/NotificationToast.hpp"
#include "ui/OSDOverlay.hpp"
#include "services/NotesService.hpp"
#include "ui/NotesOverlay.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/ImageFileFilters.hpp"
#include "ui/launcher/LauncherOverlay.hpp"
#include "services/LauncherService.hpp"
#include "ui/wallpaper/WallpaperController.hpp"
#include "ui/wallpaper/WallpaperBackend.hpp"

#include <gtk/gtk.h>
#include <iostream>
#include <memory>
#include <ctime>

namespace realmheart::ui {

class ShellRuntime {
public:
    ShellRuntime(
        GtkApplication* application,
        wallpaper::WallpaperBackendType wallpaper_backend
    )
        : application_(application),
          requested_wallpaper_backend_(wallpaper_backend),
          notification_server_(notification_history_),
          notification_daemon_(notification_server_, notification_history_) {

        battery_ = std::make_unique<services::BatteryService>();
        media_ = std::make_unique<services::MediaService>();
        notes_service_ = std::make_unique<services::NotesService>();
        utilities_ = std::make_unique<services::UtilityManager>();
        session_ = std::make_unique<services::SessionManager>();
        launcher_service_ = std::make_unique<services::LauncherService>();

        notification_server_.set_notification_handler([this](const auto& entry) {
            if (toast_) toast_->show(entry, 5000);
        });

        if (!notification_daemon_.start()) {
            std::cerr << "Unable to start Realmheart notification daemon\n";
        }
    }

    ~ShellRuntime() {
        // Stop callbacks that capture this before tearing down any UI owners.
        notification_server_.set_notification_handler({});
        notification_daemon_.stop();

        wallpaper_controller_.reset();
        launcher_overlay_.reset();
        notes_overlay_.reset();
        toast_.reset();
        osd_.reset();

        if (sidebar_) {
            GtkWindow* sidebar_window = GTK_WINDOW(sidebar_->get_window());
            // Join module workers before destroying the widgets they may update.
            sidebar_.reset();
            gtk_window_destroy(sidebar_window);
        }
        if (hotspot_ != nullptr) {
            gtk_window_destroy(hotspot_);
            hotspot_ = nullptr;
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

        std::string current_path = utilities_->load_wallpaper_path();
        if (!current_path.empty()) {
            (void)wallpaper_controller_->set_wallpaper(current_path);
        }
    }

    void toggle_right_sidebar() {
        ensure_initialized();
        state_.toggle_right_sidebar();
        apply_right_sidebar_visibility();
    }

    void show_osd_volume() {
        ensure_initialized();
        if (auto audio = services::Audio::read_default_sink()) {
            osd_->show_volume(audio->volume * 100.0);
        }
    }

    void show_osd_brightness() {
        ensure_initialized();
        if (auto bri = services::Brightness::read()) {
            osd_->show_brightness(bri->percent);
        }
    }

    void toggle_bar() {
        ensure_initialized();
        state_.toggle_bar();
        apply_bar_visibility();
    }

    void take_screenshot_full() {
        utilities_->take_screenshot_full("/home/zahed/Pictures/Screenshots/full_" + std::to_string(time(nullptr)) + ".png");
    }

    void take_screenshot_area() {
        utilities_->take_screenshot_area_to_clipboard();
    }

    void extract_ocr_area() {
        utilities_->extract_text_from_area();
    }

    void launch_launcher() {
        ensure_initialized();
        launcher_overlay_->toggle();
    }

    void set_wallpaper(const std::string& path = "") {
        ensure_initialized();
        if (path.empty()) {
            choose_wallpaper_native();
        } else {
            std::string error_message;
            if (wallpaper_controller_->set_wallpaper(path, &error_message)) {
                // Success: Now update the persistence state
                services::WallpaperService* service = utilities_->get_wallpaper_service();
                if (service) {
                    service->update_state(path);
                }
            } else {
                std::cerr << "Unable to set wallpaper: " << error_message << '\n';
            }
        }
    }

    void switch_wallpaper_backend(const std::string& backend_name) {
        ensure_initialized();
        const auto backend = wallpaper::parse_wallpaper_backend_type(backend_name);
        if (!backend) {
            std::cerr << "Unknown wallpaper backend: " << backend_name
                      << " (expected gtk or native)\n";
            return;
        }

        std::string error_message;
        if (!wallpaper_controller_->switch_backend(*backend, &error_message)) {
            std::cerr << "Unable to switch wallpaper backend to "
                      << backend_name << ": " << error_message << '\n';
            return;
        }

        requested_wallpaper_backend_ = *backend;
        std::cerr << "Wallpaper backend switched to " << backend_name << '\n';
    }

    void choose_wallpaper_native() {
        GtkFileDialog* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Choose Wallpaper");

        GListModel* filters = create_image_file_filters();
        gtk_file_dialog_set_filters(dialog, filters);
        g_object_unref(filters);

        gtk_file_dialog_open(dialog, nullptr, nullptr,
            +[](GObject* source, GAsyncResult* res, gpointer data) {
                auto* runtime = static_cast<ShellRuntime*>(data);
                GError* error = nullptr;
                GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &error);
                if (file) {
                    char* path = g_file_get_path(file);
                    if (path) {
                        runtime->set_wallpaper(path);
                        g_free(path);
                    }
                    g_object_unref(file);
                } else if (error) {
                    g_error_free(error);
                }
            }, this);
        g_object_unref(dialog);
    }

    void generate_theme() {
        // Use the current persisted wallpaper path for color generation
        std::string path = utilities_->load_wallpaper_path();
        if (!path.empty()) {
            utilities_->generate_colors(path);
        }
    }

    void start_recording() {
        std::string path = "/home/zahed/Videos/Recordings/rec_" + std::to_string(time(nullptr)) + ".mp4";
        utilities_->start_recording(path);
    }

    void stop_recording() {
        utilities_->stop_recording();
    }

    void toggle_notes() {
        ensure_initialized();
        notes_overlay_->toggle();
    }

    void lock_session() {
        session_->lock();
    }

    void open_logout_menu() {
        session_->logout_menu();
    }

    void quit() {
        g_application_quit(G_APPLICATION(application_));
    }
    void restart() {
        // Kill current instance and launch a new one
        // We use a detached process to ensure the new instance starts after this one exits.
        const auto backend = wallpaper_controller_ != nullptr
            ? wallpaper_controller_->active_backend()
            : requested_wallpaper_backend_;
        std::string cmd = "realmheart --shell --wallpaper-backend ";
        cmd += wallpaper::wallpaper_backend_type_name(backend);
        if (fork() == 0) {
            setsid();
            execlp("sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(1);
        }
        g_application_quit(G_APPLICATION(application_));
    }

private:
    void ensure_initialized() {
        if (!toast_) toast_ = std::make_unique<NotificationToast>(application_);
        if (!osd_) osd_ = std::make_unique<OSDOverlay>(application_);
        if (!notes_overlay_) {
            notes_overlay_ = std::make_unique<NotesOverlay>(application_, notes_service_.get());
        }
        if (bar_ == nullptr) {
            bar_ = bar::present_vertical_bar(
                application_, notification_history_, [this] { toggle_right_sidebar(); }
            );
        }
        if (!sidebar_) {
            sidebar_ = std::make_unique<sidebar::RightSidebar>(application_, notification_history_);
            gtk_widget_set_visible(sidebar_->get_window(), FALSE);
        }
        if (hotspot_ == nullptr) {
            hotspot_ = GTK_WINDOW(gtk_application_window_new(application_));
            gtk_window_set_decorated(hotspot_, FALSE);
            gtk_window_set_default_size(hotspot_, 16, 16);
            LayerSurfaceSpec spec;
            spec.surface_namespace = "realmheart-right-hotspot";
            spec.layer = LayerSurfaceLevel::Overlay;
            spec.anchor_right = true;
            spec.anchor_top = true;
            apply_layer_surface(hotspot_, spec);
            GtkWidget* button = gtk_button_new();
            gtk_widget_set_tooltip_text(button, "Open Realmheart controls");
            g_signal_connect(button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<ShellRuntime*>(data)->toggle_right_sidebar();
            }), this);
            gtk_window_set_child(hotspot_, button);
            gtk_window_present(hotspot_);
        }
        if (!launcher_overlay_) {
            launcher_overlay_ = std::make_unique<ui::LauncherOverlay>(application_, *launcher_service_);
        }
        if (!wallpaper_controller_) {
            wallpaper_controller_ = std::make_unique<wallpaper::WallpaperController>(
                application_,
                requested_wallpaper_backend_
            );
            std::string error_message;
            if (!wallpaper_controller_->initialize(&error_message)) {
                std::cerr << "Unable to initialize wallpaper backend: "
                          << error_message << '\n';
            }
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
    wallpaper::WallpaperBackendType requested_wallpaper_backend_ = wallpaper::WallpaperBackendType::Gtk;
    services::NotificationHistory notification_history_;
    services::NotificationServer notification_server_;
    services::NotificationDaemon notification_daemon_;
    std::unique_ptr<services::UtilityManager> utilities_;
    std::unique_ptr<services::SessionManager> session_;
    std::unique_ptr<services::BatteryService> battery_;
    std::unique_ptr<services::MediaService> media_;
    std::unique_ptr<services::NotesService> notes_service_;
    std::unique_ptr<ui::NotesOverlay> notes_overlay_;
    std::unique_ptr<NotificationToast> toast_;
    std::unique_ptr<OSDOverlay> osd_;
    GtkWindow* bar_ = nullptr;
    GtkWindow* hotspot_ = nullptr;
    std::unique_ptr<sidebar::RightSidebar> sidebar_;
    std::unique_ptr<ui::LauncherOverlay> launcher_overlay_;
    std::unique_ptr<services::LauncherService> launcher_service_;
    std::unique_ptr<wallpaper::WallpaperController> wallpaper_controller_;
    ShellState state_;
};

void activate_shell(GtkApplication* /*app*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->activate();
}

void toggle_right_sidebar_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->toggle_right_sidebar();
}

void show_osd_volume_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->show_osd_volume();
}

void show_osd_brightness_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->show_osd_brightness();
}

void toggle_bar_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->toggle_bar();
}

void take_screenshot_full_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->take_screenshot_full();
}

void take_screenshot_area_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->take_screenshot_area();
}

void extract_ocr_area_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->extract_ocr_area();
}

void launch_launcher_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->launch_launcher();
}

void set_wallpaper_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->set_wallpaper();
}

void set_wallpaper_path_action(GSimpleAction* /*action*/, GVariant* parameter, gpointer user_data) {
    if (parameter == nullptr) return;
    static_cast<ShellRuntime*>(user_data)->set_wallpaper(g_variant_get_string(parameter, nullptr));
}

void set_wallpaper_backend_action(
    GSimpleAction* /*action*/,
    GVariant* parameter,
    gpointer user_data
) {
    if (parameter == nullptr) return;
    static_cast<ShellRuntime*>(user_data)->switch_wallpaper_backend(
        g_variant_get_string(parameter, nullptr)
    );
}

void generate_theme_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->generate_theme();
}

void start_recording_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->start_recording();
}

void stop_recording_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->stop_recording();
}

void toggle_notes_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->toggle_notes();
}

void lock_session_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->lock_session();
}

void open_logout_menu_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->open_logout_menu();
}

void restart_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->restart();
}

void quit_action(GSimpleAction* /*action*/, GVariant* /*parameter*/, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->quit();
}

constexpr GActionEntry kShellActions[] = {
    {"sidebar-right-toggle", toggle_right_sidebar_action, nullptr, nullptr, nullptr, {}},
    {"bar-toggle", toggle_bar_action, nullptr, nullptr, nullptr, {}},
    {"osd-volume", show_osd_volume_action, nullptr, nullptr, nullptr, {}},
    {"osd-brightness", show_osd_brightness_action, nullptr, nullptr, nullptr, {}},
    {"lock-session", lock_session_action, nullptr, nullptr, nullptr, {}},
    {"logout-menu", open_logout_menu_action, nullptr, nullptr, nullptr, {}},
    {"restart", restart_action, nullptr, nullptr, nullptr, {}},
    {"screenshot-full", take_screenshot_full_action, nullptr, nullptr, nullptr, {}},
    {"screenshot-area", take_screenshot_area_action, nullptr, nullptr, nullptr, {}},
    {"extract-ocr", extract_ocr_area_action, nullptr, nullptr, nullptr, {}},
    {"start-recording", start_recording_action, nullptr, nullptr, nullptr, {}},
    {"stop-recording", stop_recording_action, nullptr, nullptr, nullptr, {}},
    {"toggle-notes", toggle_notes_action, nullptr, nullptr, nullptr, {}},
    {"set-wallpaper", set_wallpaper_action, nullptr, nullptr, nullptr, {}},
    {"set-wallpaper-path", set_wallpaper_path_action, "s", nullptr, nullptr, {}},
    {"set-wallpaper-backend", set_wallpaper_backend_action, "s", nullptr, nullptr, {}},
    {"generate-theme", generate_theme_action, nullptr, nullptr, nullptr, {}},
    {"launch-launcher", launch_launcher_action, nullptr, nullptr, nullptr, {}},
    {"quit", quit_action, nullptr, nullptr, nullptr, {}},
};

int run_shell(wallpaper::WallpaperBackendType wallpaper_backend) {
    GtkApplication* application = gtk_application_new(
        realmheart::core::shell_application_id().data(),
        G_APPLICATION_DEFAULT_FLAGS
);
    auto runtime = std::make_unique<ShellRuntime>(application, wallpaper_backend);

    g_action_map_add_action_entries(
        G_ACTION_MAP(application),
        kShellActions,
        static_cast<gint>(sizeof(kShellActions) / sizeof(kShellActions[0])),
        runtime.get()
    );
    g_signal_connect(application, "activate", G_CALLBACK(activate_shell), runtime.get());

    int status = g_application_run(G_APPLICATION(application), 0, nullptr);

    // ShellRuntime owns GTK windows and callbacks; destroy it while the
    // GtkApplication is still alive.
    runtime.reset();
    g_object_unref(application);
    return status;
}

} // namespace realmheart::ui
