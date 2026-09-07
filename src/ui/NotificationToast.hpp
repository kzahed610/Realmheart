#pragma once

#include "services/Notifications.hpp"

#include <deque>
#include <functional>
#include <gtk/gtk.h>
#include <memory>

namespace realmheart::ui::bar::widgets {
class ThemedSvgIcon;
}

namespace realmheart::ui {

class NotificationToast {
public:
    explicit NotificationToast(GtkApplication* app, int monitor_index = -1);
    ~NotificationToast();

    using CloseHandler = std::function<bool(std::uint32_t, std::uint32_t)>;

    void show(const services::NotificationEntry& entry, int timeout_ms);
    void close(std::uint32_t id, std::uint32_t reason);
    void set_close_handler(CloseHandler handler) { close_handler_ = std::move(handler); }
    void dismiss();

    GtkWidget* get_window() const { return window_; }

private:
    struct QueuedToast {
        services::NotificationEntry entry;
        int timeout_ms = 4000;
    };

    void show_next();
    void hide_current();
    void update_current(const QueuedToast& toast);
    void request_close(std::uint32_t reason);
    void schedule_timeout();

    static gboolean dismiss_timeout(gpointer data);

    GtkApplication* app_ = nullptr;
    int monitor_index_ = -1;
    GtkWidget* window_ = nullptr;
    GtkWidget* reveal_ = nullptr;
    GtkWidget* label_app_ = nullptr;
    GtkWidget* label_summary_ = nullptr;
    GtkWidget* label_body_ = nullptr;
    GtkWidget* close_button_ = nullptr;
    std::unique_ptr<bar::widgets::ThemedSvgIcon> icon_;

    guint timeout_id_ = 0;
    int current_timeout_ms_ = 4000;
    std::uint32_t current_id_ = 0;
    std::deque<QueuedToast> queue_;
    CloseHandler close_handler_;
    bool visible_ = false;
    bool closing_ = false;
    bool destroying_ = false;
};

} // namespace realmheart::ui
