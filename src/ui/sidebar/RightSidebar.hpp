#pragma once

#include "services/KeepAwake.hpp"
#include "services/Notifications.hpp"
#include "ui/components/BaseWidget.hpp"

#include <atomic>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <mutex>
#include <cstdint>
#include <vector>

namespace realmheart::ui::components {
class LabelWidget;
}

namespace realmheart::ui::sidebar {

class RightSidebar {
public:
    RightSidebar(
        GtkApplication* app,
        services::NotificationHistory& notification_history,
        std::function<void(double)> show_volume_osd = {},
        std::function<void(double)> show_brightness_osd = {}
    );
    ~RightSidebar();

    RightSidebar(const RightSidebar&) = delete;
    RightSidebar& operator=(const RightSidebar&) = delete;

    void add_module(std::unique_ptr<components::BaseWidget> module);
    void refresh();
    GtkWidget* get_window() const { return window_; }

private:
    struct AsyncUiState {
        std::atomic<bool> alive{true};
        std::atomic<std::uint64_t> power_profile_generation{0};
        std::mutex power_profile_mutex;
        components::LabelWidget* power_profile_label = nullptr; // GTK main thread only
    };

    void setup_layout();
    void populate_modules();

    GtkApplication* app_ = nullptr;
    GtkWidget* window_ = nullptr;
    GtkWidget* container_ = nullptr;
    std::vector<std::unique_ptr<components::BaseWidget>> modules_;
    std::shared_ptr<services::KeepAwake> keep_awake_;
    services::NotificationHistory& notification_history_;
    std::function<void(double)> show_volume_osd_;
    std::function<void(double)> show_brightness_osd_;
    std::shared_ptr<AsyncUiState> async_ui_state_ = std::make_shared<AsyncUiState>();
};

} // namespace realmheart::ui::sidebar
