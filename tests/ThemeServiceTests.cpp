#include "services/ThemeService.hpp"

#include <cstdlib>
#include <iostream>
#include <utility>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    realmheart::services::ThemeService service;
    int calls = 0;
    std::string last_primary;

    auto subscription = service.subscribe([&](const auto& palette) {
        ++calls;
        last_primary = palette.get("primary");
    });

    realmheart::services::Palette first;
    first.colors["primary"] = "#123456";
    service.update_palette(first);
    require(calls == 1, "subscriber did not receive palette");
    require(last_primary == "#123456", "subscriber received wrong palette");

    auto moved = std::move(subscription);
    realmheart::services::Palette second;
    second.colors["primary"] = "#abcdef";
    service.update_palette(second);
    require(calls == 2, "moved subscription stopped receiving updates");

    moved.reset();
    realmheart::services::Palette third;
    third.colors["primary"] = "#ffffff";
    service.update_palette(third);
    require(calls == 2, "reset subscription still received updates");

    std::cout << "ThemeServiceTests passed\n";
    return 0;
}
