#pragma once

namespace realmheart::animation::layered {

class SpringMotion {
public:
    SpringMotion(double frequency_hz = 1.0, double damping_ratio = 0.8);

    void configure(double frequency_hz, double damping_ratio);
    void reset(double value = 0.0, double velocity = 0.0);
    void set_target(double target);
    void advance(double delta_seconds);

    [[nodiscard]] double value() const;
    [[nodiscard]] double velocity() const;
    [[nodiscard]] double target() const;
    [[nodiscard]] bool settled(double epsilon = 0.001) const;

private:
    double frequency_hz_ = 1.0;
    double damping_ratio_ = 0.8;
    double value_ = 0.0;
    double velocity_ = 0.0;
    double target_ = 0.0;
};

} // namespace realmheart::animation::layered
