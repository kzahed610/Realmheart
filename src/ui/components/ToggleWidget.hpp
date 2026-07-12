#pragma once

#include "ui/components/BaseWidget.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace realmheart::ui::components {

class ToggleWidget : public BaseWidget {
public:
    ToggleWidget(std::string label, bool initial, std::function<bool(bool)> on_toggle);
    ~ToggleWidget() override;

    GtkWidget* get_widget() override;
    void set_active(bool active);

private:
    struct WorkerState {
        std::mutex mutex;
        std::condition_variable cv;
        std::function<bool(bool)> on_toggle;
        bool shutdown = false;
        bool has_pending = false;
        bool target_state = false;
        std::atomic<bool> alive{true};
        GtkWidget* switch_widget = nullptr; // GTK main thread only
        gulong signal_handler = 0;
    };

    void start_worker();

    GtkWidget* box_ = nullptr;
    GtkWidget* switch_ = nullptr;
    bool updating_ = false;
    std::shared_ptr<WorkerState> worker_state_;
    std::thread worker_;
};

} // namespace realmheart::ui::components
