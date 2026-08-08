#include "worldscar/WorldscarSelection.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

namespace realmheart::worldscar {
namespace {

TEST(WorldscarSelectionTests, ExcludesCommittedWallpaperAndStartsAfterIt) {
    const std::vector<std::filesystem::path> library{
        "/walls/a.jpg",
        "/walls/b.jpg",
        "/walls/c.jpg",
        "/walls/d.jpg",
    };

    const auto selection = WorldscarSelection::create(library, "/walls/b.jpg");
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection->candidate_count(), 3U);
    EXPECT_EQ(selection->selected(), std::filesystem::path("/walls/c.jpg"));

    const auto preview = selection->preview();
    EXPECT_EQ(preview.previous, std::filesystem::path("/walls/a.jpg"));
    EXPECT_EQ(preview.selected, std::filesystem::path("/walls/c.jpg"));
    EXPECT_EQ(preview.next, std::filesystem::path("/walls/d.jpg"));
    EXPECT_TRUE(preview.previous_visible);
    EXPECT_TRUE(preview.next_visible);
    EXPECT_FALSE(preview.previous_far_visible);
    EXPECT_FALSE(preview.next_far_visible);
}

TEST(WorldscarSelectionTests, FiveCandidatesExposeInvisibleFarNeighbours) {
    const std::vector<std::filesystem::path> library{
        "/walls/current.jpg",
        "/walls/a.jpg",
        "/walls/b.jpg",
        "/walls/c.jpg",
        "/walls/d.jpg",
        "/walls/e.jpg",
    };
    const auto selection = WorldscarSelection::create(
        library,
        "/walls/current.jpg"
    );
    ASSERT_TRUE(selection.has_value());
    const auto preview = selection->preview();
    EXPECT_EQ(preview.selected, std::filesystem::path("/walls/a.jpg"));
    EXPECT_EQ(preview.previous, std::filesystem::path("/walls/e.jpg"));
    EXPECT_EQ(preview.next, std::filesystem::path("/walls/b.jpg"));
    EXPECT_EQ(preview.previous_far, std::filesystem::path("/walls/d.jpg"));
    EXPECT_EQ(preview.next_far, std::filesystem::path("/walls/c.jpg"));
    EXPECT_TRUE(preview.previous_far_visible);
    EXPECT_TRUE(preview.next_far_visible);
}

TEST(WorldscarSelectionTests, NavigationWrapsBothDirections) {
    const std::vector<std::filesystem::path> library{
        "/walls/a.jpg",
        "/walls/b.jpg",
        "/walls/c.jpg",
        "/walls/d.jpg",
    };
    auto selection = WorldscarSelection::create(library, "/walls/a.jpg");
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection->selected(), std::filesystem::path("/walls/b.jpg"));

    ASSERT_TRUE(selection->navigate(-1));
    EXPECT_EQ(selection->selected(), std::filesystem::path("/walls/d.jpg"));

    ASSERT_TRUE(selection->navigate(1));
    EXPECT_EQ(selection->selected(), std::filesystem::path("/walls/b.jpg"));
}

TEST(WorldscarSelectionTests, TwoCandidatesShowOneDistinctNeighbour) {
    const std::vector<std::filesystem::path> library{
        "/walls/current.jpg",
        "/walls/a.jpg",
        "/walls/b.jpg",
    };
    const auto selection = WorldscarSelection::create(
        library,
        "/walls/current.jpg"
    );
    ASSERT_TRUE(selection.has_value());
    const auto preview = selection->preview();
    EXPECT_TRUE(preview.previous_visible);
    EXPECT_FALSE(preview.next_visible);
    EXPECT_NE(preview.previous, preview.selected);
}

TEST(WorldscarSelectionTests, OneImageLibraryRemainsSafe) {
    const std::vector<std::filesystem::path> library{
        "/walls/only.jpg",
    };
    auto selection = WorldscarSelection::create(library, "/walls/only.jpg");
    ASSERT_TRUE(selection.has_value());
    EXPECT_EQ(selection->candidate_count(), 1U);
    EXPECT_EQ(selection->selected(), std::filesystem::path("/walls/only.jpg"));
    EXPECT_FALSE(selection->preview().previous_visible);
    EXPECT_FALSE(selection->preview().next_visible);
    EXPECT_TRUE(selection->navigate(1));
    EXPECT_EQ(selection->selected(), std::filesystem::path("/walls/only.jpg"));
}

} // namespace
} // namespace realmheart::worldscar
