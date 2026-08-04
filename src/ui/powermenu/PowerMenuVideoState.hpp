#pragma once

namespace realmheart::ui::powermenu {

enum class PowerMenuVideoPhase {
    Hidden,
    Opening,
    Visible,
    Closing,
};

class PowerMenuVideoState {
public:
    void present();
    void dismiss();
    void hide_immediately();
    void advance(double delta_seconds);

    [[nodiscard]] PowerMenuVideoPhase phase() const;
    [[nodiscard]] double opacity() const;
    [[nodiscard]] bool media_required() const;
    [[nodiscard]] bool needs_frame() const;

private:
    void sample();

    PowerMenuVideoPhase phase_ = PowerMenuVideoPhase::Hidden;
    double progress_ = 0.0;
    double opacity_ = 0.0;
};

} // namespace realmheart::ui::powermenu
