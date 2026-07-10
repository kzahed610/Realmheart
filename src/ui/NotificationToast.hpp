#pragma once

#include <gtk/gtk.h>
#include <string>
#include <memory>
#include "services/Notifications.hpp"

namespace realmheart::ui {

class NotificationToast {
public:
    explicit NotificationToast(GtkApplication* app);
    ~NotificationToast();

    void show(const services::NotificationEntry& entry, int timeout_ms);
    void dismiss();

    GtkWidget* get_window() const { return window_; }

private:
    void on_timeout() { dismiss(); }

    GtkApplication* app_;
    GtkWidget* window_ = nullptr;
    GtkWidget* label_summary_ = nullptr;
    GtkWidget* label_body_ = nullptr;
    guint timeout_id_ = 0;
};

} // namespace realmheart::ui
