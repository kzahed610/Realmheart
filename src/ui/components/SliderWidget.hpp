#pragma once

#include "ui/components/BaseWidget.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace realmheart::ui::components {

class SliderWidget : public BaseWidget {
public:
    using Mutator = std::function<std::optional<double>(double)>;
    using ConfirmedCallback = std::function<void(double)>;

    SliderWidget(
        std::string label,
        double min,
        double max,
        double initial,
        Mutator on_change,
        ConfirmedCallback on_confirmed = {}
    );
    ~SliderWidget() override;

    GtkWidget* get_widget() override;
    void set_value(double value);
    void set_available(bool available);

private:
    struct AsyncState {
        std::atomic<bool> alive{true};
        std::atomic<std::uint64_t> generation{0};
        std::mutex mutation_mutex;
        GtkWidget* scale = nullptr; // GTK main thread only
        GtkWidget* value_label = nullptr; // GTK main thread only
        gulong value_changed_handler = 0;
        Mutator on_change;
        ConfirmedCallback on_confirmed;
        std::atomic<double> confirmed_value{0.0};
    };

    GtkWidget* box_ = nullptr;
    GtkWidget* scale_ = nullptr;
    GtkWidget* value_label_ = nullptr;
    bool updating_ = false;
    bool available_ = true;
    double pending_value_ = 0.0;
    guint debounce_source_ = 0;
    std::shared_ptr<AsyncState> state_;
};

} // namespace realmheart::ui::components
