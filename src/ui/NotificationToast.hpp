#pragma once

#include "services/Notifications.hpp"

#include <deque>
#include <gtk/gtk.h>

namespace realmheart::ui {

class NotificationToast {
public:
    explicit NotificationToast(GtkApplication* app);
    ~NotificationToast();

    void show(const services::NotificationEntry& entry, int timeout_ms);
    void dismiss();

    GtkWidget* get_window() const { return window_; }

private:
    struct QueuedToast {
        services::NotificationEntry entry;
        int timeout_ms = 5000;
    };

    void show_next();
    void hide_current();

    GtkApplication* app_;
    GtkWidget* window_ = nullptr;
    GtkWidget* label_summary_ = nullptr;
    GtkWidget* label_body_ = nullptr;
    guint timeout_id_ = 0;
    std::deque<QueuedToast> queue_;
    bool visible_ = false;
    bool destroying_ = false;
};

} // namespace realmheart::ui
