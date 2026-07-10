#pragma once

#include <gtk/gtk.h>
#include <string>
#include <memory>
#include "services/Notifications.hpp"

namespace realmheart::ui {

class OSDOverlay {
public:
    explicit OSDOverlay(GtkApplication* app);
    ~OSDOverlay();

    void show_volume(double percent);
    void show_brightness(double percent);
    void dismiss();

private:
    void update_label(const std::string& text, const std::string& icon);

    GtkApplication* app_;
    GtkWidget* window_ = nullptr;
    GtkWidget* label_ = nullptr;
    guint timeout_id_ = 0;
};

} // namespace realmheart::ui
