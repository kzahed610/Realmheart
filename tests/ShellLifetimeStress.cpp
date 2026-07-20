#include "ui/ShellApp.hpp"
#include "ui/wallpaper/WallpaperBackend.hpp"

#include <charconv>
#include <iostream>
#include <string_view>

namespace {

bool parse_positive_int(std::string_view text, int& value) {
    int parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed
    );
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed <= 0) {
        return false;
    }
    value = parsed;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    int iterations = 20;
    auto backend = realmheart::ui::wallpaper::WallpaperBackendType::Gtk;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--iterations") {
            if (index + 1 >= argc || !parse_positive_int(argv[++index], iterations)) {
                std::cerr << "--iterations requires a positive integer\n";
                return 2;
            }
            continue;
        }
        if (argument == "--wallpaper-backend") {
            if (index + 1 >= argc) {
                std::cerr << "--wallpaper-backend requires gtk or native\n";
                return 2;
            }
            const auto parsed = realmheart::ui::wallpaper::parse_wallpaper_backend_type(
                argv[++index]
            );
            if (!parsed) {
                std::cerr << "invalid wallpaper backend\n";
                return 2;
            }
            backend = *parsed;
            continue;
        }
        std::cerr << "unknown argument: " << argument << '\n';
        return 2;
    }

    return realmheart::ui::run_shell_lifetime_stress(backend, iterations);
}
