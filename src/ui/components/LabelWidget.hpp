#pragma once

#include "ui/components/BaseWidget.hpp"
#include <gtk/gtk.h>
#include <string>
#include <functional>
#include <atomic>

namespace realmheart::ui::components {

class LabelWidget : public ThemeableWidget {
public:
    using Reader = std::function<std::string()>;

    LabelWidget(const std::string& label, const std::string& initial_value, Reader reader = {});
    ~LabelWidget() override = default;

    GtkWidget* get_widget() override;
    void refresh() override;
    void set_value(const std::string& value);
    void apply_theme(const services::Palette& palette) override;

private:
    GtkWidget* box_ = nullptr;
    GtkWidget* val_label_ = nullptr;
    Reader reader_;
    std::atomic<bool> refresh_in_flight_ = false;
};

} // namespace realmheart::ui::components
