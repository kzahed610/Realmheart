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

struct SliderLayout {
    int row_height = 36;
    int row_spacing = 7;
    int icon_size = 17;
    int label_width = 69;
    int value_width = 34;
};

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
    void set_layout(SliderLayout layout);
    [[nodiscard]] std::uint64_t refresh_generation() const noexcept;
    void apply_refresh(std::optional<double> value, std::uint64_t generation);

private:
    void show_interaction_feedback();

    struct AsyncState {
        std::atomic<bool> alive{true};
        std::atomic<std::uint64_t> generation{0};
        std::atomic<bool> mutation_pending{false};
        std::mutex mutation_mutex;
        GtkWidget* scale = nullptr; // GTK main thread only
        GtkWidget* value_label = nullptr; // GTK main thread only
        gulong value_changed_handler = 0;
        Mutator on_change;
        ConfirmedCallback on_confirmed;
        std::atomic<double> confirmed_value{0.0};
    };

    GtkWidget* box_ = nullptr;
    GtkWidget* icon_ = nullptr;
    GtkWidget* name_label_ = nullptr;
    GtkWidget* scale_ = nullptr;
    GtkWidget* value_label_ = nullptr;
    bool updating_ = false;
    bool available_ = true;
    double pending_value_ = 0.0;
    std::uint64_t pending_generation_ = 0;
    guint debounce_source_ = 0;
    guint interaction_feedback_source_ = 0;
    std::shared_ptr<AsyncState> state_;
};

} // namespace realmheart::ui::components
