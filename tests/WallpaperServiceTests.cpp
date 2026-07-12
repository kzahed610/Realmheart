#include "services/WallpaperService.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <unistd.h>

namespace {

class WallpaperServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "realmheart-wallpaper-service-XXXXXX").string();
        char* created = ::mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root_ = created;
        state_file_ = root_ / "state" / "wallpaper" / "path.txt";
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path create_file(const std::string& name) {
        const auto path = root_ / name;
        std::ofstream(path, std::ios::binary) << "fixture";
        return path;
    }

    std::filesystem::path root_;
    std::filesystem::path state_file_;
};

TEST_F(WallpaperServiceTest, RejectsMissingPath) {
    realmheart::services::WallpaperService service(state_file_);

    EXPECT_FALSE(service.validate_image(root_ / "missing.png"));
}

TEST_F(WallpaperServiceTest, RejectsDirectory) {
    realmheart::services::WallpaperService service(state_file_);

    EXPECT_FALSE(service.validate_image(root_));
}

TEST_F(WallpaperServiceTest, AcceptsSupportedExtensionCaseInsensitively) {
    realmheart::services::WallpaperService service(state_file_);
    const auto image = create_file("wallpaper.JpEg");

    EXPECT_TRUE(service.validate_image(image));
}

TEST_F(WallpaperServiceTest, RejectsVideoExtension) {
    realmheart::services::WallpaperService service(state_file_);
    const auto video = create_file("wallpaper.mp4");

    EXPECT_FALSE(service.validate_image(video));
}

TEST_F(WallpaperServiceTest, PersistedPathRoundTrips) {
    realmheart::services::WallpaperService service(state_file_);
    const auto image = create_file("wallpaper.png");

    ASSERT_TRUE(service.persist_path(image));
    EXPECT_EQ(service.load_path(), std::optional<std::filesystem::path>{image});
}

TEST_F(WallpaperServiceTest, EmptyPersistedStateReturnsNoWallpaper) {
    std::filesystem::create_directories(state_file_.parent_path());
    {
        std::ofstream empty_state(state_file_);
    }
    realmheart::services::WallpaperService service(state_file_);

    EXPECT_EQ(service.load_path(), std::nullopt);
}

TEST_F(WallpaperServiceTest, InvalidPersistedStateReturnsNoWallpaper) {
    std::filesystem::create_directories(state_file_.parent_path());
    std::ofstream(state_file_) << (root_ / "deleted.webp").string() << '\n';
    realmheart::services::WallpaperService service(state_file_);

    EXPECT_EQ(service.load_path(), std::nullopt);
}

} // namespace
