#pragma once

#include "services/BatteryService.hpp"
#include "ui/bar/widgets/BarIconButton.hpp"

#include <functional>
#include <gtk/gtk.h>
#include <optional>

namespace realmheart::ui::bar::widgets {

class BatteryWidget {
public:
    explicit BatteryWidget(std::function<void(GtkPopover*)> request_exclusive_open);
    ~BatteryWidget();

    BatteryWidget(const BatteryWidget&) = delete;
    BatteryWidget& operator=(const BatteryWidget&) = delete;

    GtkWidget* widget() const { return button_.widget(); }
    void update(const std::optional<services::BatteryStatus>& status);
    void close();

private:
    void show_held();
    void hide_held();
    void update_popup();
    static const char* icon_for(const services::BatteryStatus& status);

    std::function<void(GtkPopover*)> request_exclusive_open_;
    BarIconButton button_;
    GtkWidget* popover_ = nullptr;
    GtkWidget* percentage_label_ = nullptr;
    GtkWidget* state_label_ = nullptr;
    GtkWidget* rate_label_ = nullptr;
    std::optional<services::BatteryStatus> status_;
};

} // namespace realmheart::ui::bar::widgets
