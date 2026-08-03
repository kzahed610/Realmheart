#pragma once

#include "ui/powermenu/PowerMenuLayout.hpp"

#include <array>
#include <functional>
#include <gtk/gtk.h>
#include <optional>

namespace realmheart::ui::powermenu {

class PowerMenuControls {
public:
    using Action = PowerMenuAction;
    using ActionCallback = std::function<void(Action)>;
    using DismissCallback = std::function<void()>;

    PowerMenuControls(ActionCallback on_action, DismissCallback on_dismiss);

    PowerMenuControls(const PowerMenuControls&) = delete;
    PowerMenuControls& operator=(const PowerMenuControls&) = delete;

    [[nodiscard]] GtkWidget* widget() const;
    void focus_first();
    [[nodiscard]] bool prepare_for_interaction();
    void set_armed(std::optional<Action> action);

private:
    void update_layout();
    void draw(cairo_t* cr, int width, int height) const;
    void queue_draw();
    void sync_animation_targets();
    void ensure_animation_tick();
    [[nodiscard]] bool advance_animations(GdkFrameClock* frame_clock);
    [[nodiscard]] double animation_amount(std::size_t index) const;
    [[nodiscard]] bool contains_action(double x, double y) const;

    GtkWidget* root_ = nullptr;
    GtkWidget* canvas_ = nullptr;
    GtkWidget* button_layer_ = nullptr;
    std::array<GtkWidget*, 5> buttons_{};
    std::array<double, 5> animation_progress_{};
    std::array<double, 5> animation_target_{};
    guint animation_tick_id_ = 0;
    gint64 last_animation_frame_time_ = 0;
    std::optional<Action> armed_action_;
    ActionCallback on_action_;
    DismissCallback on_dismiss_;
};

} // namespace realmheart::ui::powermenu
