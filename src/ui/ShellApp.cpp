#include "ui/ShellApp.hpp"

#include "core/ShellControl.hpp"
#include "services/Audio.hpp"
#include "services/BatteryService.hpp"
#include "services/Brightness.hpp"
#include "services/LauncherService.hpp"
#include "services/MediaService.hpp"
#include "services/NotesService.hpp"
#include "services/NotificationDaemon.hpp"
#include "services/NotificationServer.hpp"
#include "services/Notifications.hpp"
#include "services/SessionManager.hpp"
#include "services/ThemeService.hpp"
#include "services/UtilityManager.hpp"
#include "ui/ImageFileFilters.hpp"
#include "ui/LayerSurface.hpp"
#include "ui/NotesOverlay.hpp"
#include "ui/NotificationToast.hpp"
#include "ui/OSDOverlay.hpp"
#include "ui/ShellState.hpp"
#include "ui/ThemeStyles.hpp"
#include "ui/bar/VerticalBar.hpp"
#include "ui/launcher/LauncherOverlay.hpp"
#include "ui/sidebar/RightSidebar.hpp"
#include "ui/wallpaper/WallpaperBackend.hpp"
#include "ui/wallpaper/WallpaperController.hpp"

#include <gtk/gtk.h>

#include <array>
#include <cerrno>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace realmheart::ui {
namespace {

std::string current_executable_path() {
    std::array<char, 4096> buffer{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) return {};
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}

std::filesystem::path user_media_directory(GUserDirectory directory, const char* fallback_name) {
    if (const char* configured = g_get_user_special_dir(directory);
        configured != nullptr && *configured != '\0') {
        return configured;
    }
    if (const char* home = g_get_home_dir(); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / fallback_name;
    }
    return std::filesystem::temp_directory_path() / "realmheart" / fallback_name;
}

} // namespace

class ShellRuntime {
public:
    ShellRuntime(
        GtkApplication* application,
        wallpaper::WallpaperBackendType wallpaper_backend
    )
        : application_(application),
          requested_wallpaper_backend_(wallpaper_backend),
          notification_server_(notification_history_),
          notification_daemon_(notification_server_, notification_history_),
          theme_service_(std::make_shared<services::ThemeService>()),
          utilities_(std::make_unique<services::UtilityManager>(theme_service_)),
          session_(std::make_unique<services::SessionManager>()),
          battery_(std::make_unique<services::BatteryService>()),
          media_(std::make_unique<services::MediaService>()),
          notes_service_(std::make_unique<services::NotesService>()),
          launcher_service_(std::make_unique<services::LauncherService>()) {
        notification_server_.set_notification_handler([this](const auto& entry) {
            if (toast_) toast_->show(entry, 5000);
        });

        if (!notification_daemon_.start()) {
            std::cerr << "Unable to start Realmheart notification daemon\n";
        }
    }

    ~ShellRuntime() {
        // Stop callbacks that capture this before tearing down UI/controllers.
        notification_server_.set_notification_handler({});
        notification_daemon_.stop();

        wallpaper_controller_.reset();
        launcher_overlay_.reset();
        notes_overlay_.reset();
        toast_.reset();
        osd_.reset();

        sidebar_.reset();
        bar_.reset();

        if (hotspot_ != nullptr) {
            gtk_window_destroy(hotspot_);
            hotspot_ = nullptr;
        }

        // Unsubscribe/remove the display-wide CSS provider while GTK is alive.
        theme_styles_.reset();
    }

    void activate() {
        ensure_initialized();
        state_.show_bar();
        apply_bar_visibility();

        const std::string current_path = utilities_->load_wallpaper_path();
        if (current_path.empty()) return;

        std::string error_message;
        if (!wallpaper_controller_->set_wallpaper(current_path, &error_message)) {
            std::cerr << "Unable to restore wallpaper: " << error_message << '\n';
            return;
        }
        generate_theme_for(current_path);
    }

    void toggle_right_sidebar() {
        ensure_initialized();
        state_.toggle_right_sidebar();
        apply_right_sidebar_visibility();
    }

    void show_osd_volume_value(double value) {
        ensure_initialized();
        osd_->show_volume(value);
    }

    void show_osd_brightness_value(double value) {
        ensure_initialized();
        osd_->show_brightness(value);
    }

    void show_osd_volume() {
        ensure_initialized();
        if (const auto audio = services::Audio::read_default_sink()) {
            show_osd_volume_value(audio->volume * 100.0);
        }
    }

    void show_osd_brightness() {
        ensure_initialized();
        if (const auto brightness = services::Brightness::read()) {
            show_osd_brightness_value(brightness->percent);
        }
    }

    void toggle_bar() {
        ensure_initialized();
        state_.toggle_bar();
        apply_bar_visibility();
    }

    void take_screenshot_full() {
        const auto path = user_media_directory(G_USER_DIRECTORY_PICTURES, "Pictures") /
            "Screenshots" /
            ("full_" + std::to_string(std::time(nullptr)) + ".png");
        utilities_->take_screenshot_full(path.string());
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

    void set_wallpaper(const std::string& path = {}) {
        ensure_initialized();
        if (path.empty()) {
            choose_wallpaper_native();
            return;
        }

        std::string error_message;
        if (!wallpaper_controller_->set_wallpaper(path, &error_message)) {
            std::cerr << "Unable to set wallpaper: " << error_message << '\n';
            return;
        }

        if (services::WallpaperService* service = utilities_->get_wallpaper_service()) {
            if (!service->update_state(path)) {
                std::cerr << "Wallpaper changed, but its path could not be persisted\n";
            }
        }

        // Generate from the exact image that was just accepted by the renderer.
        // Previously this was only called at startup, leaving the fallback palette
        // unchanged for every later wallpaper swap.
        generate_theme_for(path);
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

        gtk_file_dialog_open(
            dialog,
            nullptr,
            nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                auto* runtime = static_cast<ShellRuntime*>(data);
                GError* error = nullptr;
                GFile* file = gtk_file_dialog_open_finish(
                    GTK_FILE_DIALOG(source),
                    result,
                    &error
                );
                if (file != nullptr) {
                    char* path = g_file_get_path(file);
                    if (path != nullptr) {
                        runtime->set_wallpaper(path);
                        g_free(path);
                    }
                    g_object_unref(file);
                } else if (error != nullptr) {
                    if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
                        std::cerr << "Wallpaper picker failed: " << error->message << '\n';
                    }
                    g_error_free(error);
                }
            },
            this
        );
        g_object_unref(dialog);
    }

    void generate_theme() {
        generate_theme_for(utilities_->load_wallpaper_path());
    }

    void start_recording() {
        const auto path = user_media_directory(G_USER_DIRECTORY_VIDEOS, "Videos") /
            "Recordings" /
            ("rec_" + std::to_string(std::time(nullptr)) + ".mp4");
        utilities_->start_recording(path.string());
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
        const auto backend = wallpaper_controller_ != nullptr
            ? wallpaper_controller_->active_backend()
            : requested_wallpaper_backend_;
        const std::string executable = current_executable_path();
        if (executable.empty()) {
            std::cerr << "Unable to restart Realmheart: /proc/self/exe could not be resolved: "
                      << std::strerror(errno) << '\n';
            return;
        }

        const std::string old_pid = std::to_string(::getpid());
        const std::string backend_name(
            wallpaper::wallpaper_backend_type_name(backend)
        );

        const pid_t helper = ::fork();
        if (helper < 0) {
            std::cerr << "Unable to restart Realmheart: fork failed: "
                      << std::strerror(errno) << '\n';
            return;
        }
        if (helper == 0) {
            static_cast<void>(::setsid());
            ::execl(
                executable.c_str(),
                executable.c_str(),
                "--restart-helper",
                old_pid.c_str(),
                backend_name.c_str(),
                static_cast<char*>(nullptr)
            );
            _exit(127);
        }

        g_application_quit(G_APPLICATION(application_));
    }

private:
    void generate_theme_for(const std::string& path) {
        if (path.empty()) return;
        if (!utilities_->generate_colors(path)) {
            std::cerr << "[Theme] Keeping the current palette because generation failed\n";
        }
    }

    void ensure_initialized() {
        if (!theme_styles_) {
            theme_styles_ = std::make_unique<ThemeStyles>(theme_service_);
        }
        if (!toast_) toast_ = std::make_unique<NotificationToast>(application_);
        if (!osd_) osd_ = std::make_unique<OSDOverlay>(application_);
        if (!notes_overlay_) {
            notes_overlay_ = std::make_unique<NotesOverlay>(application_, notes_service_.get());
        }
        if (!bar_) {
            bar_ = std::make_unique<bar::VerticalBar>(
                application_,
                notification_history_,
                *battery_,
                *media_,
                [this] { toggle_right_sidebar(); }
            );
        }
        if (!sidebar_) {
            sidebar_ = std::make_unique<sidebar::RightSidebar>(
                application_,
                notification_history_,
                [this](double value) { show_osd_volume_value(value); },
                [this](double value) { show_osd_brightness_value(value); }
            );
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
            g_signal_connect(
                button,
                "clicked",
                G_CALLBACK(+[](GtkButton*, gpointer data) {
                    static_cast<ShellRuntime*>(data)->toggle_right_sidebar();
                }),
                this
            );
            gtk_window_set_child(hotspot_, button);
            gtk_window_present(hotspot_);
        }
        if (!launcher_overlay_) {
            launcher_overlay_ = std::make_unique<LauncherOverlay>(
                application_,
                *launcher_service_
            );
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
            // Hide, don't destroy: the controller and its workers remain valid.
            gtk_widget_set_visible(window, FALSE);
        }
    }

    void apply_bar_visibility() {
        GtkWidget* window = bar_->get_window();
        if (state_.bar_visible()) {
            bar_->refresh();
            gtk_window_present(GTK_WINDOW(window));
        } else {
            gtk_widget_set_visible(window, FALSE);
        }
    }

    GtkApplication* application_ = nullptr;
    wallpaper::WallpaperBackendType requested_wallpaper_backend_ =
        wallpaper::WallpaperBackendType::Gtk;

    services::NotificationHistory notification_history_;
    services::NotificationServer notification_server_;
    services::NotificationDaemon notification_daemon_;

    std::shared_ptr<services::ThemeService> theme_service_;
    std::unique_ptr<services::UtilityManager> utilities_;
    std::unique_ptr<services::SessionManager> session_;
    std::unique_ptr<services::BatteryService> battery_;
    std::unique_ptr<services::MediaService> media_;
    std::unique_ptr<services::NotesService> notes_service_;
    std::unique_ptr<services::LauncherService> launcher_service_;

    std::unique_ptr<ThemeStyles> theme_styles_;
    std::unique_ptr<NotesOverlay> notes_overlay_;
    std::unique_ptr<NotificationToast> toast_;
    std::unique_ptr<OSDOverlay> osd_;
    std::unique_ptr<bar::VerticalBar> bar_;
    GtkWindow* hotspot_ = nullptr;
    std::unique_ptr<sidebar::RightSidebar> sidebar_;
    std::unique_ptr<LauncherOverlay> launcher_overlay_;
    std::unique_ptr<wallpaper::WallpaperController> wallpaper_controller_;

    ShellState state_;
};

namespace {

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

void take_screenshot_full_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->take_screenshot_full();
}

void take_screenshot_area_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->take_screenshot_area();
}

void extract_ocr_area_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->extract_ocr_area();
}

void launch_launcher_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->launch_launcher();
}

void set_wallpaper_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->set_wallpaper();
}

void set_wallpaper_path_action(GSimpleAction*, GVariant* parameter, gpointer user_data) {
    if (parameter == nullptr) return;
    static_cast<ShellRuntime*>(user_data)->set_wallpaper(
        g_variant_get_string(parameter, nullptr)
    );
}

void set_wallpaper_backend_action(GSimpleAction*, GVariant* parameter, gpointer user_data) {
    if (parameter == nullptr) return;
    static_cast<ShellRuntime*>(user_data)->switch_wallpaper_backend(
        g_variant_get_string(parameter, nullptr)
    );
}

void generate_theme_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->generate_theme();
}

void start_recording_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->start_recording();
}

void stop_recording_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->stop_recording();
}

void toggle_notes_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->toggle_notes();
}

void lock_session_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->lock_session();
}

void open_logout_menu_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->open_logout_menu();
}

void restart_action(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<ShellRuntime*>(user_data)->restart();
}

void quit_action(GSimpleAction*, GVariant*, gpointer user_data) {
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

} // namespace

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
    g_signal_connect(
        application,
        "activate",
        G_CALLBACK(activate_shell),
        runtime.get()
    );

    const int status = g_application_run(G_APPLICATION(application), 0, nullptr);

    // Controllers own GTK windows and callbacks; destroy them while the
    // GtkApplication/display are still valid.
    runtime.reset();
    g_object_unref(application);
    return status;
}

} // namespace realmheart::ui
