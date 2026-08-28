// tests/WallpaperLibraryTests.cpp
#include "mana_core/WallpaperLibrary.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace {

class WallpaperLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "realmheart-wallpaper-library-XXXXXX").string();
        char* created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    void create_binary(const std::string& name, std::initializer_list<unsigned char> bytes) {
        const auto path = root_ / name;
        std::ofstream stream(path, std::ios::binary);
        for (unsigned char byte : bytes) {
            stream.put(static_cast<char>(byte));
        }
    }

    std::filesystem::path root_;
};

TEST_F(WallpaperLibraryTest, DefaultRootResolvesToPicturesWallpapers) {
    const char* home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);
    EXPECT_EQ(
        realmheart::mana_core::WallpaperLibrary::default_root(),
        std::filesystem::path(home) / "Pictures" / "Wallpapers"
    );
}

TEST_F(WallpaperLibraryTest, DiscoversSupportedImagesAndIgnoresOthers) {
    realmheart::mana_core::WallpaperLibrary library;

    // Minimal valid magic headers (plausible_image probes magic, not full decode)
    create_binary("a.png", {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00});
    create_binary("B.JpEg", {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x00, 0x00, 0x00});
    create_binary("webp.webp", {'R', 'I', 'F', 'F', 0x00, 0x00, 0x00, 0x00, 'W', 'E', 'B', 'P'});

    create_binary("notes.txt", {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'});
    create_binary("video.mp4", {0x00, 0x00, 0x00, 0x18, 0x66, 0x74, 0x79, 0x70});
    create_binary("fake.png", {'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g', '!'});

    auto discovery = library.discover(root_);

    ASSERT_EQ(discovery.paths.size(), 3u);
    EXPECT_EQ(discovery.paths[0].filename(), "a.png");
    EXPECT_EQ(discovery.paths[1].filename(), "B.JpEg");
    EXPECT_EQ(discovery.paths[2].filename(), "webp.webp");
    for (const auto& path : discovery.paths) {
        EXPECT_TRUE(path.is_absolute());
    }
}

TEST_F(WallpaperLibraryTest, MissingRootProducesDiagnosticNotCrash) {
    realmheart::mana_core::WallpaperLibrary library;
    auto discovery = library.discover(root_ / "does-not-exist");

    EXPECT_TRUE(discovery.paths.empty());
    EXPECT_FALSE(discovery.diagnostics.empty());
}

} // namespace