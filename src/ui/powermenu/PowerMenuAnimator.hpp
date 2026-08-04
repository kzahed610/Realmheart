#pragma once

#include "ui/powermenu/PowerMenuManifest.hpp"

#include <cstdint>
#include <vector>

namespace realmheart::ui::powermenu {

enum class PowerMenuScenePhase { Hidden, Opening, Idle, Confirming, Closing };
enum class PowerMenuBlinkState { Open, Half, Closed };

struct PowerMenuLayerMotionSample {
    double translation_x = 0.0;
    double translation_y = 0.0;
    double rotation_degrees = 0.0;
    double scale = 1.0;
    double opacity = 1.0;
    double macro_displacement = 0.0;
    double flow_displacement = 0.0;
};

struct PowerMenuFrame {
    double scene_opacity = 0.0;
    double scene_scale = 0.985;
    PowerMenuBlinkState blink = PowerMenuBlinkState::Open;
    double iris_glow = 0.0;
    double rune_glow = 0.0;
    std::vector<PowerMenuLayerMotionSample> layers;
};

class PowerMenuAnimator {
public:
    explicit PowerMenuAnimator(const PowerMenuRig& rig, std::uint64_t seed = 0x52484dU);

    void open();
    void close();
    void set_confirming(bool confirming);
    void advance(double delta_seconds);

    [[nodiscard]] PowerMenuScenePhase phase() const;
    [[nodiscard]] bool needs_frame() const;
    [[nodiscard]] const PowerMenuFrame& frame() const;

private:
    [[nodiscard]] PowerMenuBlinkState sample_blink(double elapsed) const;
    [[nodiscard]] double blink_interval(std::uint64_t index) const;
    [[nodiscard]] bool double_blink(std::uint64_t index) const;
    void sample_frame();

    PowerMenuRig rig_;
    std::uint64_t seed_ = 0;
    PowerMenuScenePhase phase_ = PowerMenuScenePhase::Hidden;
    double lifecycle_progress_ = 0.0;
    double visible_elapsed_ = 0.0;
    PowerMenuFrame frame_;
};

} // namespace realmheart::ui::powermenu
