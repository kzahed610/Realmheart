#pragma once

#include "services/SystemMonitorService.hpp"
#include "ui/bar/widgets/ThemedSvgIcon.hpp"

#include <atomic>
#include <functional>
#include <gtk/gtk.h>
#include <memory>
#include <vector>

namespace realmheart::ui::bar::widgets {

class SystemMonitorWidget {
public:
    explicit SystemMonitorWidget(std::function<void(GtkPopover*)> request_exclusive_open);
    ~SystemMonitorWidget();

    SystemMonitorWidget(const SystemMonitorWidget&) = delete;
    SystemMonitorWidget& operator=(const SystemMonitorWidget&) = delete;

    GtkWidget* widget() const { return button_; }
    void close();

private:
    struct AsyncState {
        std::atomic<bool> alive{true};
        std::atomic<bool> in_flight{false};
        SystemMonitorWidget* owner = nullptr;
    };

    struct UsageRow {
        GtkWidget* value = nullptr;
        GtkWidget* progress = nullptr;
    };

    void toggle();
    void open();
    void handle_closed();
    void stop_live_refresh();
    void request_sample();
    void apply(const std::optional<services::SystemUsageSnapshot>& snapshot);
    void trigger_click_feedback();
    UsageRow add_row(GtkWidget* parent, const char* name);

    std::function<void(GtkPopover*)> request_exclusive_open_;
    GtkWidget* button_ = nullptr;
    GtkWidget* popover_ = nullptr;
    GtkWidget* state_label_ = nullptr;
    std::vector<std::unique_ptr<ThemedSvgIcon>> metric_icons_;
    UsageRow cpu_;
    UsageRow memory_;
    UsageRow swap_;
    UsageRow gpu_;
    std::shared_ptr<AsyncState> async_state_ = std::make_shared<AsyncState>();
    guint refresh_timer_id_ = 0;
    guint click_feedback_timer_id_ = 0;
    bool open_ = false;
};

} // namespace realmheart::ui::bar::widgets
