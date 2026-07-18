#pragma once

#include "services/Notifications.hpp"

#include <deque>
#include <gtk/gtk.h>
#include <memory>

namespace realmheart::ui::bar::widgets {
class ThemedSvgIcon;
}

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
        int timeout_ms = 4000;
    };

    void show_next();
    void hide_current();
    void schedule_timeout();

    static gboolean dismiss_timeout(gpointer data);

    GtkApplication* app_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* reveal_ = nullptr;
    GtkWidget* label_app_ = nullptr;
    GtkWidget* label_summary_ = nullptr;
    GtkWidget* label_body_ = nullptr;
    GtkWidget* close_button_ = nullptr;
    std::unique_ptr<bar::widgets::ThemedSvgIcon> icon_;

    guint timeout_id_ = 0;
    int current_timeout_ms_ = 4000;
    std::deque<QueuedToast> queue_;
    bool visible_ = false;
    bool closing_ = false;
    bool destroying_ = false;
};

} // namespace realmheart::ui
