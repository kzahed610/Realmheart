#pragma once

namespace realmheart::effects {

enum class TransitionState {
    Hidden,
    Opening,
    Visible,
    Closing,
};

struct TransitionDurations {
    double open_seconds = 0.30;
    double close_seconds = 0.18;
};

class TransitionTimeline {
public:
    explicit TransitionTimeline(TransitionDurations durations = {});

    void open() noexcept;
    void close() noexcept;
    void toggle() noexcept;

    void snap_hidden() noexcept;
    void snap_visible() noexcept;

    [[nodiscard]] bool advance(double elapsed_seconds) noexcept;

    [[nodiscard]] TransitionState state() const noexcept;
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool target_visible() const noexcept;
    [[nodiscard]] TransitionDurations durations() const noexcept;

private:
    [[nodiscard]] static double sanitize_duration(double seconds) noexcept;

    TransitionDurations durations_;
    TransitionState state_ = TransitionState::Hidden;
    double progress_ = 0.0;
};

} // namespace realmheart::effects
