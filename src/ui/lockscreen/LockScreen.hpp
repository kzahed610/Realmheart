#pragma once
#include <gtk/gtk.h>
#include <string>
#include <memory>

namespace realmheart::ui {

class LockScreen {
public:
    explicit LockScreen(GtkApplication* app);
    ~LockScreen() = default;

    GtkWidget* get_window() const { return window_; }
    void lock() { is_locked_ = true; }
    void unlock() { is_locked_ = false; }

private:
    void setup_layout();
    void apply_layer_shell();

    GtkApplication* app_;
    GtkWidget* window_ = nullptr;
    GtkWidget* overlay_container_ = nullptr;
    GtkWidget* clock_label_ = nullptr;
    bool is_locked_ = true;
};

} // namespace realmheart::ui
