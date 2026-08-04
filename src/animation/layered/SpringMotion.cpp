#include "animation/layered/SpringMotion.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace realmheart::animation::layered {

SpringMotion::SpringMotion(double frequency_hz, double damping_ratio) {
    configure(frequency_hz, damping_ratio);
}

void SpringMotion::configure(double frequency_hz, double damping_ratio) {
    frequency_hz_ = std::isfinite(frequency_hz)
        ? std::clamp(frequency_hz, 0.01, 60.0) : 1.0;
    damping_ratio_ = std::isfinite(damping_ratio)
        ? std::clamp(damping_ratio, 0.0, 4.0) : 0.8;
}

void SpringMotion::reset(double value, double velocity) {
    value_ = std::isfinite(value) ? value : 0.0;
    velocity_ = std::isfinite(velocity) ? velocity : 0.0;
    target_ = value_;
}

void SpringMotion::set_target(double target) {
    if (std::isfinite(target)) target_ = target;
}

void SpringMotion::advance(double delta_seconds) {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0) return;
    const double dt = std::min(delta_seconds, 0.25);
    const double omega = 2.0 * std::numbers::pi * frequency_hz_;
    const double y0 = value_ - target_;
    const double v0 = velocity_;
    const double zeta = damping_ratio_;

    double y = y0;
    double velocity = v0;
    if (zeta < 1.0 - 1e-6) {
        const double damped = omega * std::sqrt(1.0 - (zeta * zeta));
        const double envelope = std::exp(-zeta * omega * dt);
        const double a = y0;
        const double b = (v0 + (zeta * omega * y0)) / damped;
        const double cosine = std::cos(damped * dt);
        const double sine = std::sin(damped * dt);
        const double wave = (a * cosine) + (b * sine);
        y = envelope * wave;
        velocity = envelope * (
            (-zeta * omega * wave) +
            (-a * damped * sine) + (b * damped * cosine)
        );
    } else if (zeta <= 1.0 + 1e-6) {
        const double envelope = std::exp(-omega * dt);
        const double b = v0 + (omega * y0);
        y = envelope * (y0 + (b * dt));
        velocity = envelope * (b - (omega * (y0 + (b * dt))));
    } else {
        const double root = std::sqrt((zeta * zeta) - 1.0);
        const double r1 = -omega * (zeta - root);
        const double r2 = -omega * (zeta + root);
        const double c1 = (v0 - (r2 * y0)) / (r1 - r2);
        const double c2 = y0 - c1;
        const double e1 = std::exp(r1 * dt);
        const double e2 = std::exp(r2 * dt);
        y = (c1 * e1) + (c2 * e2);
        velocity = (c1 * r1 * e1) + (c2 * r2 * e2);
    }

    value_ = target_ + y;
    velocity_ = velocity;
    if (settled(1e-7)) {
        value_ = target_;
        velocity_ = 0.0;
    }
}

double SpringMotion::value() const { return value_; }
double SpringMotion::velocity() const { return velocity_; }
double SpringMotion::target() const { return target_; }

bool SpringMotion::settled(double epsilon) const {
    const double threshold = std::max(std::abs(epsilon), 1e-9);
    return std::abs(value_ - target_) <= threshold &&
        std::abs(velocity_) <= threshold;
}

} // namespace realmheart::animation::layered
