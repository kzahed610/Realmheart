#pragma once

#include <gio/gio.h>

#include <string>
#include <utility>

namespace realmheart::ui::powermenu {

// Owns the short-lived power-menu renderer process. Keeping the video decoder,
// GStreamer GL workers and ripple GtkGLArea outside the persistent shell lets
// the kernel reclaim their entire address space when the menu closes.
class PowerMenuProcess {
public:
    explicit PowerMenuProcess(std::string helper_override = {})
        : helper_override_(std::move(helper_override)) {}
    ~PowerMenuProcess();

    PowerMenuProcess(const PowerMenuProcess&) = delete;
    PowerMenuProcess& operator=(const PowerMenuProcess&) = delete;

    void toggle(int monitor_index, double normalized_origin_x, double normalized_origin_y);
    void close() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    [[nodiscard]] bool launch(
        int monitor_index,
        double normalized_origin_x,
        double normalized_origin_y
    );
    void request_close() noexcept;
    void escalate_shutdown() noexcept;
    void reap_child(int status) noexcept;
    [[nodiscard]] std::string helper_executable() const;

    static gboolean control_read_callback(gint fd, GIOCondition condition, gpointer data);
    static void child_watch_callback(GPid pid, gint status, gpointer data);

    GPid child_pid_ = 0;
    int control_fd_ = -1;
    guint child_watch_id_ = 0;
    guint control_watch_id_ = 0;
    guint startup_timeout_id_ = 0;
    guint shutdown_timeout_id_ = 0;
    bool terminate_sent_ = false;
    bool ready_ = false;
    std::string helper_override_;
    std::string control_buffer_;
};

} // namespace realmheart::ui::powermenu
