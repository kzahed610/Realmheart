#pragma once

#include "services/SystemMonitorService.hpp"
#include "ui/bar/widgets/BarIconButton.hpp"

#include <atomic>
#include <functional>
#include <gtk/gtk.h>
#include <memory>

namespace realmheart::ui::bar::widgets {

class SystemMonitorWidget {
public:
    explicit SystemMonitorWidget(std::function<void(GtkPopover*)> request_exclusive_open);
    ~SystemMonitorWidget();

    SystemMonitorWidget(const SystemMonitorWidget&) = delete;
    SystemMonitorWidget& operator=(const SystemMonitorWidget&) = delete;

    GtkWidget* widget() const { return button_.widget(); }
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

    void show_held();
    void hide_held();
    void request_sample();
    void apply(const std::optional<services::SystemUsageSnapshot>& snapshot);
    UsageRow add_row(GtkWidget* parent, const char* name);

    std::function<void(GtkPopover*)> request_exclusive_open_;
    BarIconButton button_;
    GtkWidget* popover_ = nullptr;
    GtkWidget* state_label_ = nullptr;
    UsageRow cpu_;
    UsageRow memory_;
    UsageRow swap_;
    UsageRow gpu_;
    std::shared_ptr<AsyncState> async_state_ = std::make_shared<AsyncState>();
    guint refresh_timer_id_ = 0;
    bool held_ = false;
};

} // namespace realmheart::ui::bar::widgets
