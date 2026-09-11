#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace realmheart::eventd {

enum class SourceTrust { Realmheart, User, UntrustedLocal };

std::string to_string(SourceTrust trust);

class RateLimiter {
public:
    struct Decision {
        bool allowed = true;
        std::string reason;
    };

    Decision allow_create(const std::string& source_id, SourceTrust trust, std::size_t active_count);
    Decision allow_update(const std::string& source_id, SourceTrust trust);
    [[nodiscard]] std::size_t rate_limited_sources() const;

private:
    struct SourceState {
        std::deque<std::chrono::steady_clock::time_point> creates;
        std::deque<std::chrono::steady_clock::time_point> updates;
        bool limited = false;
    };

    static void prune(std::deque<std::chrono::steady_clock::time_point>& samples,
                      std::chrono::steady_clock::time_point now);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, SourceState> sources_;
    std::deque<std::chrono::steady_clock::time_point> global_creates_;
};

} // namespace realmheart::eventd
