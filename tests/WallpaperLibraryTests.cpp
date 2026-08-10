#include "relictombs/WallpaperLibrary.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace realmheart::relictombs {
namespace {

class WallpaperLibraryTest : public ::testing::Test {
protected:
    void SetUp() override {
        GError* error = nullptr;
        gchar* created = g_dir_make_tmp(
            "realmheart-relictombs-library-XXXXXX",
            &error
        );
        ASSERT_NE(created, nullptr)
            << (error != nullptr && error->message != nullptr
                    ? error->message
                    : "unable to create temporary directory");
        g_clear_error(&error);
        root_ = created;
        g_free(created);
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path create_png(const std::filesystem::path& relative) {
        const auto path = root_ / relative;
        std::filesystem::create_directories(path.parent_path());

        GdkPixbuf* pixbuf = gdk_pixbuf_new(
            GDK_COLORSPACE_RGB,
            TRUE,
            8,
            2,
            2
        );
        EXPECT_NE(pixbuf, nullptr);
        if (pixbuf == nullptr) return {};
        gdk_pixbuf_fill(pixbuf, 0x5b3fd6ffU);

        GError* error = nullptr;
        const gboolean saved = gdk_pixbuf_save(
            pixbuf,
            path.c_str(),
            "png",
            &error,
            nullptr
        );
        EXPECT_TRUE(saved)
            << (error != nullptr && error->message != nullptr
                    ? error->message
                    : "unable to save test png");
        g_clear_error(&error);
        g_object_unref(pixbuf);
        return path;
    }

    std::filesystem::path root_;
    WallpaperLibrary library_;
};

TEST_F(WallpaperLibraryTest, MissingRootReturnsNoCandidates) {
    const auto result = library_.discover(root_ / "missing");
    EXPECT_TRUE(result.paths.empty());
    EXPECT_FALSE(result.diagnostics.empty());
}

TEST_F(WallpaperLibraryTest, IncludesDirectImagesAndExcludesNestedImages) {
    const auto direct = create_png("Arthur.png");
    create_png("TBATE/Regis.png");

    const auto result = library_.discover(root_);
    ASSERT_EQ(result.paths.size(), 1U);
    EXPECT_EQ(result.paths.front(), direct);
}

TEST_F(WallpaperLibraryTest, RejectsUnsupportedAndCorruptFiles) {
    create_png("valid.png");
    {
        std::ofstream(root_ / "not-an-image.png") << "this is not png data";
        std::ofstream(root_ / "video.mp4") << "not relevant";
    }

    const auto result = library_.discover(root_);
    ASSERT_EQ(result.paths.size(), 1U);
    EXPECT_EQ(result.paths.front().filename(), "valid.png");
    EXPECT_FALSE(result.diagnostics.empty());
}

TEST_F(WallpaperLibraryTest, HandlesUppercaseSpacesAndUnicodeNames) {
    const auto upper = create_png("BETA.PNG");
    const auto spaced = create_png("Arthur Leywin.png");
    const auto unicode = create_png("世界.png");

    const auto result = library_.discover(root_);
    EXPECT_EQ(result.paths.size(), 3U);
    EXPECT_NE(std::find(result.paths.begin(), result.paths.end(), upper), result.paths.end());
    EXPECT_NE(std::find(result.paths.begin(), result.paths.end(), spaced), result.paths.end());
    EXPECT_NE(std::find(result.paths.begin(), result.paths.end(), unicode), result.paths.end());
}

TEST_F(WallpaperLibraryTest, SortsCaseInsensitivelyWithStableTieBreak) {
    create_png("zeta.png");
    create_png("Beta.png");
    create_png("alpha.png");
    create_png("ALPHA.PNG");

    const auto result = library_.discover(root_);
    ASSERT_EQ(result.paths.size(), 4U);

    std::vector<std::string> names;
    for (const auto& path : result.paths) names.push_back(path.filename().string());
    EXPECT_EQ(
        names,
        (std::vector<std::string>{"ALPHA.PNG", "alpha.png", "Beta.png", "zeta.png"})
    );
}

TEST_F(WallpaperLibraryTest, IncludesDirectFileSymlinkButDoesNotTraverseDirectorySymlink) {
    const auto target = create_png("target.png");
    create_png("nested/hidden.png");

    std::error_code error;
    std::filesystem::create_symlink(target, root_ / "linked.png", error);
    ASSERT_FALSE(error) << error.message();
    std::filesystem::create_directory_symlink(root_ / "nested", root_ / "linked-dir", error);
    ASSERT_FALSE(error) << error.message();

    const auto result = library_.discover(root_);
    ASSERT_EQ(result.paths.size(), 2U);
    EXPECT_EQ(result.paths[0].filename(), "linked.png");
    EXPECT_EQ(result.paths[1].filename(), "target.png");
}

} // namespace
} // namespace realmheart::relictombs
