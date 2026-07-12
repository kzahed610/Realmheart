#pragma once

#include "ui/components/BaseWidget.hpp"
#include <gtk/gtk.h>
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace realmheart::ui::components {

class ToggleWidget : public ThemeableWidget {
public:
    ToggleWidget(const std::string& label, bool initial, std::function<bool(bool)> on_toggle);
    ~ToggleWidget() override;

    GtkWidget* get_widget() override;
    void refresh() override;
    void set_active(bool active);
    void apply_theme(const services::Palette& palette) override;

private:
    struct WorkerState {
        std::mutex mutex;
        std::condition_variable cv;
        std::function<bool(bool)> on_toggle;
        bool shutdown = false;
        bool has_pending = false;
        bool target_state = false;
    };

    GtkWidget* box_ = nullptr;
    GtkWidget* switch_ = nullptr;
    bool updating_ = false;
    std::shared_ptr<WorkerState> worker_state_;
    std::thread worker_;
};

} // namespace realmheart::ui::components
