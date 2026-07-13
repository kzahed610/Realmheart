#pragma once

#include "ui/components/BaseWidget.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace realmheart::ui::components {

class LabelWidget : public BaseWidget {
public:
    using Reader = std::function<std::string()>;

    LabelWidget(std::string label, std::string initial_value, Reader reader = {});
    ~LabelWidget() override;

    GtkWidget* get_widget() override;
    void refresh() override;
    void set_value(const std::string& value);

private:
    struct AsyncState {
        std::atomic<bool> alive{true};
        std::atomic<bool> refresh_in_flight{false};
        std::atomic<std::uint64_t> generation{0};
        GtkWidget* value_label = nullptr; // GTK main thread only
    };

    GtkWidget* box_ = nullptr;
    Reader reader_;
    std::shared_ptr<AsyncState> state_;
};

} // namespace realmheart::ui::components
