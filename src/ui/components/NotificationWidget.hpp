#pragma once

#include "services/Notifications.hpp"
#include "ui/components/BaseWidget.hpp"

#include <atomic>
#include <memory>

namespace realmheart::ui::components {

class NotificationWidget : public BaseWidget {
public:
    explicit NotificationWidget(services::NotificationHistory& history);
    ~NotificationWidget() override;

    GtkWidget* get_widget() override;
    void refresh() override;

private:
    struct LifetimeState {
        std::atomic<bool> alive{true};
        NotificationWidget* owner = nullptr; // GTK main thread only
        std::atomic<bool> refresh_queued{false};
    };

    void clear_notifications();
    void dismiss_notification(std::uint32_t id);
    void begin_drag_scroll(
        GtkGestureDrag* gesture,
        double start_x,
        double start_y
    );
    void update_drag_scroll(GtkGestureDrag* gesture, double offset_x, double offset_y);
    void end_drag_scroll();
    void begin_row_swipe(GtkGestureDrag* gesture, double start_x, double start_y);
    void update_row_swipe(GtkGestureDrag* gesture, double offset_x, double offset_y);
    void end_row_swipe(GtkGestureDrag* gesture);
    void animate_swipe_release(double target, bool dismiss_after);
    void set_swipe_translate(double offset);
    static gboolean swipe_tick(GtkWidget* widget, GdkFrameClock* frame_clock, gpointer data);
    void cancel_swipe_animation();

    GtkWidget* box_ = nullptr;
    GtkWidget* scroller_ = nullptr;
    GtkWidget* list_ = nullptr;
    GtkWidget* count_label_ = nullptr;
    GtkWidget* clear_button_ = nullptr;
    GtkAdjustment* vertical_adjustment_ = nullptr;
    double drag_start_value_ = 0.0;
    bool drag_active_ = false;
    bool drag_blocked_ = false;
    // Horizontal swipe-to-dismiss state (one active row at a time).
    GtkWidget* swipe_row_ = nullptr;
    std::uint32_t swipe_id_ = 0;
    double swipe_offset_ = 0.0;
    bool swipe_active_ = false;
    bool swipe_blocked_ = false;
    bool swipe_will_dismiss_ = false;
    // Swipe release animation (fly-out or spring-back).
    double swipe_anim_start_ = 0.0;
    double swipe_anim_target_ = 0.0;
    gint64 swipe_anim_start_us_ = 0;
    bool swipe_anim_dismiss_ = false;
    guint swipe_tick_id_ = 0;
    services::NotificationHistory& history_;
    std::shared_ptr<LifetimeState> state_ = std::make_shared<LifetimeState>();
    services::NotificationHistory::Subscription subscription_;
};

} // namespace realmheart::ui::components
