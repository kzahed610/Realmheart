#pragma once

#include <glib.h>

#include <string>

namespace realmheart::ui::powermenu {

// Owns the short-lived power-menu renderer process. Keeping the video decoder,
// GStreamer GL workers and ripple GtkGLArea outside the persistent shell lets
// the kernel reclaim their entire address space when the menu closes.
class PowerMenuProcess {
public:
    PowerMenuProcess() = default;
    ~PowerMenuProcess();

    PowerMenuProcess(const PowerMenuProcess&) = delete;
    PowerMenuProcess& operator=(const PowerMenuProcess&) = delete;

    void toggle(double normalized_origin_x, double normalized_origin_y);
    void close() noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    [[nodiscard]] bool launch(
        double normalized_origin_x,
        double normalized_origin_y
    );
    void request_close() noexcept;
    void reap_child(int status) noexcept;
    [[nodiscard]] std::string helper_executable() const;

    static void child_watch_callback(GPid pid, gint status, gpointer data);

    GPid child_pid_ = 0;
    int control_fd_ = -1;
    guint child_watch_id_ = 0;
};

} // namespace realmheart::ui::powermenu
