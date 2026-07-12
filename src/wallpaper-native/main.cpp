#include "wallpaper-native/NativeWallpaperRenderer.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
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
