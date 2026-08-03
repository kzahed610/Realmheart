#pragma once

#include "ui/powermenu/PowerMenuConfirmation.hpp"

#include <functional>
#include <gtk/gtk.h>
#include <memory>

namespace realmheart::ui::powermenu {

struct PowerMenuActions {
    std::function<bool()> lock;
    std::function<bool()> suspend;
    std::function<bool()> logout;
    std::function<bool()> reboot;
    std::function<bool()> power_off;
};

class PowerMenuControls;

class PowerMenuOverlay {
public:
    using Action = PowerMenuConfirmation::Action;

    PowerMenuOverlay(GtkApplication* app, PowerMenuActions actions);
    ~PowerMenuOverlay();

    PowerMenuOverlay(const PowerMenuOverlay&) = delete;
    PowerMenuOverlay& operator=(const PowerMenuOverlay&) = delete;

    void show();
    void hide();
    void toggle();
    [[nodiscard]] bool visible() const;

private:
    void activate(Action action);
    void show_confirmation(Action action);
    void clear_confirmation();
    void schedule_interaction_setup();
    void cancel_interaction_setup();
    [[nodiscard]] bool advance_interaction_setup(GtkWidget* widget);

    GtkWindow* window_ = nullptr;
    std::unique_ptr<PowerMenuControls> controls_;
    GtkWidget* confirmation_banner_ = nullptr;
    guint confirmation_timeout_id_ = 0;
    guint interaction_setup_tick_id_ = 0;
    int input_region_commit_frames_remaining_ = 0;
    PowerMenuConfirmation confirmation_;
    PowerMenuActions actions_;
};

} // namespace realmheart::ui::powermenu
