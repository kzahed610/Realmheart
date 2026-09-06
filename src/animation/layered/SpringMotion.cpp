#include "animation/layered/SpringMotion.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace realmheart::animation::layered {
namespace {

constexpr double max_state_magnitude = 1.0e9;

double bounded_state(double value, double fallback) {
    if (!std::isfinite(value)) return fallback;
    return std::clamp(value, -max_state_magnitude, max_state_magnitude);
}

double bounded_state(long double value, double fallback) {
    if (std::isnan(value)) return fallback;
    if (!std::isfinite(value)) return value < 0.0L
        ? -max_state_magnitude : max_state_magnitude;
    const long double limit = static_cast<long double>(max_state_magnitude);
    if (value <= -limit) return -max_state_magnitude;
    if (value >= limit) return max_state_magnitude;
    return static_cast<double>(value);
}

} // namespace

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
    value_ = bounded_state(value, 0.0);
    velocity_ = bounded_state(velocity, 0.0);
    target_ = value_;
}

void SpringMotion::set_target(double target) {
    if (std::isfinite(target)) target_ = bounded_state(target, target_);
}

void SpringMotion::advance(double delta_seconds) {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0) return;
    const long double dt = static_cast<long double>(delta_seconds);
    const long double omega = 2.0L * std::numbers::pi_v<long double> *
        static_cast<long double>(frequency_hz_);
    const long double y0 = static_cast<long double>(value_) -
        static_cast<long double>(target_);
    const long double v0 = static_cast<long double>(velocity_);
    const long double zeta = static_cast<long double>(damping_ratio_);

    long double y = y0;
    long double velocity = v0;
    if (zeta < 1.0L - 1e-6L) {
        const long double damped = omega * std::sqrt(1.0L - (zeta * zeta));
        const long double envelope = std::exp(-zeta * omega * dt);
        const long double a = y0;
        const long double b = (v0 + (zeta * omega * y0)) / damped;
        const long double cosine = std::cos(damped * dt);
        const long double sine = std::sin(damped * dt);
        const long double wave = (a * cosine) + (b * sine);
        y = envelope * wave;
        velocity = envelope * (
            (-zeta * omega * wave) +
            (-a * damped * sine) + (b * damped * cosine)
        );
    } else if (zeta <= 1.0L + 1e-6L) {
        const long double envelope = std::exp(-omega * dt);
        const long double b = v0 + (omega * y0);
        y = envelope * (y0 + (b * dt));
        velocity = envelope * (b - (omega * (y0 + (b * dt))));
    } else {
        const long double root = std::sqrt((zeta * zeta) - 1.0L);
        const long double r1 = -omega * (zeta - root);
        const long double r2 = -omega * (zeta + root);
        const long double c1 = (v0 - (r2 * y0)) / (r1 - r2);
        const long double c2 = y0 - c1;
        const long double e1 = std::exp(r1 * dt);
        const long double e2 = std::exp(r2 * dt);
        y = (c1 * e1) + (c2 * e2);
        velocity = (c1 * r1 * e1) + (c2 * r2 * e2);
    }

    value_ = bounded_state(static_cast<long double>(target_) + y, target_);
    velocity_ = bounded_state(velocity, 0.0);
    if (settled(1e-7)) {
        value_ = target_;
        velocity_ = 0.0;
    }
}

double SpringMotion::value() const { return value_; }
double SpringMotion::velocity() const { return velocity_; }
double SpringMotion::target() const { return target_; }

bool SpringMotion::settled(double epsilon) const {
    const double threshold = std::isfinite(epsilon)
        ? std::max(std::abs(epsilon), 1e-9) : 1e-9;
    return std::abs(static_cast<long double>(value_) -
        static_cast<long double>(target_)) <= static_cast<long double>(threshold) &&
        std::abs(static_cast<long double>(velocity_)) <= static_cast<long double>(threshold);
}

} // namespace realmheart::animation::layered
