#include "eventd/RateLimiter.hpp"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    using namespace realmheart::eventd;
    RateLimiter limiter;

    for (std::size_t index = 0; index < 30U; ++index) {
        require(limiter.allow_create("user.source", SourceTrust::User, index).allowed,
                "user source must be allowed within create budget");
    }
    require(!limiter.allow_create("user.source", SourceTrust::User, 30U).allowed,
            "user source must be throttled above create budget");
    require(limiter.rate_limited_sources() == 1U, "rate-limited source must be observable");

    RateLimiter cap_limiter;
    require(!cap_limiter.allow_create("cap.source", SourceTrust::User, 64U).allowed,
            "active-event cap must be enforced");

    RateLimiter update_limiter;
    for (std::size_t index = 0; index < 300U; ++index) {
        require(update_limiter.allow_update("updates", SourceTrust::User).allowed,
                "updates within budget must be allowed");
    }
    require(!update_limiter.allow_update("updates", SourceTrust::User).allowed,
            "updates above budget must be throttled");

    std::cout << "Event rate limiter tests passed\n";
    return 0;
}
