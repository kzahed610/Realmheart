#pragma once

#include "ui/components/BaseWidget.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace realmheart::ui::components {

class ToggleWidget : public BaseWidget {
public:
    ToggleWidget(std::string label, bool initial, std::function<bool(bool)> on_toggle);
    ~ToggleWidget() override;

    GtkWidget* get_widget() override;
    void set_active(bool active);

private:
    struct AsyncState {
        std::atomic<bool> alive{true};
        std::atomic<std::uint64_t> generation{0};
        std::mutex mutation_mutex;
        std::function<bool(bool)> on_toggle;
        GtkWidget* switch_widget = nullptr; // GTK main thread only
        gulong signal_handler = 0;
    };

    GtkWidget* box_ = nullptr;
    GtkWidget* switch_ = nullptr;
    bool updating_ = false;
    std::shared_ptr<AsyncState> state_;
};

} // namespace realmheart::ui::components
