#include "eventd/RateLimiter.hpp"

#include <algorithm>

namespace realmheart::eventd {
namespace {

struct Policy {
    std::size_t creates_per_window;
    std::size_t updates_per_window;
    std::size_t active_cap;
};

Policy policy_for(SourceTrust trust) {
    switch (trust) {
    case SourceTrust::Realmheart: return {120U, 1200U, 256U};
    case SourceTrust::User: return {30U, 300U, 64U};
    case SourceTrust::UntrustedLocal: return {10U, 100U, 32U};
    }
    return {10U, 100U, 32U};
}

constexpr auto kWindow = std::chrono::seconds(10);
constexpr std::size_t kGlobalCreatesPerWindow = 200U;

} // namespace

std::string to_string(SourceTrust trust) {
    switch (trust) {
    case SourceTrust::Realmheart: return "realmheart";
    case SourceTrust::User: return "user";
    case SourceTrust::UntrustedLocal: return "untrusted-local";
    }
    return "untrusted-local";
}

void RateLimiter::prune(
    std::deque<std::chrono::steady_clock::time_point>& samples,
    std::chrono::steady_clock::time_point now
) {
    while (!samples.empty() && now - samples.front() >= kWindow) samples.pop_front();
}

RateLimiter::Decision RateLimiter::allow_create(
    const std::string& source_id,
    SourceTrust trust,
    std::size_t active_count
) {
    const auto now = std::chrono::steady_clock::now();
    const Policy policy = policy_for(trust);
    std::lock_guard lock(mutex_);
    SourceState& state = sources_[source_id];
    prune(state.creates, now);
    prune(global_creates_, now);

    if (active_count >= policy.active_cap) {
        state.limited = true;
        return {false, "source active-event cap reached"};
    }
    if (state.creates.size() >= policy.creates_per_window) {
        state.limited = true;
        return {false, "source create rate exceeded"};
    }
    if (global_creates_.size() >= kGlobalCreatesPerWindow) {
        state.limited = true;
        return {false, "global create rate exceeded"};
    }

    state.creates.push_back(now);
    global_creates_.push_back(now);
    state.limited = false;
    return {};
}

RateLimiter::Decision RateLimiter::allow_update(const std::string& source_id, SourceTrust trust) {
    const auto now = std::chrono::steady_clock::now();
    const Policy policy = policy_for(trust);
    std::lock_guard lock(mutex_);
    SourceState& state = sources_[source_id];
    prune(state.updates, now);
    if (state.updates.size() >= policy.updates_per_window) {
        state.limited = true;
        return {false, "source update rate exceeded"};
    }
    state.updates.push_back(now);
    state.limited = false;
    return {};
}

std::size_t RateLimiter::rate_limited_sources() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::count_if(sources_.begin(), sources_.end(), [](const auto& entry) {
        return entry.second.limited;
    }));
}

} // namespace realmheart::eventd
