#include "wallpaper-native/NativeWallpaperRenderer.hpp"

#include <iostream>
#include <string>
#include <string_view>

#ifndef REALMHEART_VERSION
#define REALMHEART_VERSION "unknown"
#endif

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "Realmheart Wallpaper Renderer " << REALMHEART_VERSION << '\n';
        return 0;
    }
    if (argc != 2 || std::string(argv[1]) != "--stdio") {
        std::cerr << "Usage: realmheart-wallpaper-renderer --stdio\n";
        return 2;
    }

    realmheart::wallpaper_native::NativeWallpaperRenderer renderer;
    std::string error;
    if (!renderer.initialize(&error)) {
        std::cerr << "Native wallpaper renderer initialization failed: "
                  << error << '\n';
        return 1;
    }

    std::cout << "READY\n" << std::flush;
    return renderer.run_stdio();
}
