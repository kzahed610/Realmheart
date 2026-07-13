#include <gtest/gtest.h>
#include "services/LauncherService.hpp"

using namespace realmheart::services;

class LauncherServiceTest : public ::testing::Test {
protected:
    LauncherService service;
};

TEST_F(LauncherServiceTest, EmptyQueryReturnsEmpty) {
    auto results = service.search("", 10);
    EXPECT_TRUE(results.empty());
}

TEST_F(LauncherServiceTest, CaseInsensitiveSearch) {
    service.set_mock_index({
        {LauncherResultKind::Application, "app1", "Firefox", "Web Browser", "firefox", {"firefox"}},
        {LauncherResultKind::Application, "app2", "Foot", "Terminal", "foot", {"foot"}}
    });
    auto results = service.search("fire", 10);
    ASSERT_GE(results.size(), 1);
    EXPECT_EQ(results[0].id, "app1");
}

TEST_F(LauncherServiceTest, StableRankingExactPrefix) {
    service.set_mock_index({
        {LauncherResultKind::Application, "a", "Apple", "", "", {}},
        {LauncherResultKind::Application, "b", "Pineapple", "", "", {}}
    });
    auto results = service.search("Apple", 10);
    ASSERT_GE(results.size(), 1);
    EXPECT_EQ(results[0].id, "a");
}

TEST_F(LauncherServiceTest, StableRankingWordPrefix) {
    service.set_mock_index({
        {LauncherResultKind::Application, "a", "Google Chrome", "", "", {}},
        {LauncherResultKind::Application, "b", "Chrome", "", "", {}}
    });
    auto results = service.search("Chrome", 10);
    ASSERT_GE(results.size(), 1);
    EXPECT_EQ(results[0].id, "b");
}

TEST_F(LauncherServiceTest, StableRankingSubstring) {
    service.set_mock_index({
        {LauncherResultKind::Application, "a", "Visual Studio Code", "", "", {}},
        {LauncherResultKind::Application, "b", "Code", "", "", {}}
    });
    auto results = service.search("Code", 10);
    ASSERT_GE(results.size(), 1);
    EXPECT_EQ(results[0].id, "b");
}

TEST_F(LauncherServiceTest, RankingOrderExactOverSubstring) {
    service.set_mock_index({
        {LauncherResultKind::Application, "a", "Apple", "", "", {}},
        {LauncherResultKind::Application, "b", "Pineapple", "", "", {}}
    });
    auto results = service.search("Apple", 10);
    ASSERT_GE(results.size(), 1);
    EXPECT_EQ(results[0].id, "a");
}

TEST_F(LauncherServiceTest, ResultCountNeverExceedsLimit) {
    service.set_mock_index({
        {LauncherResultKind::Application, "a", "Alpha", "", "", {}},
        {LauncherResultKind::Application, "b", "Alphabet", "", "", {}},
        {LauncherResultKind::Application, "c", "Alphanumeric", "", "", {}}
    });
    const auto results = service.search("Al", 2);
    EXPECT_EQ(results.size(), 2U);
}

TEST_F(LauncherServiceTest, ExplicitCommandConsumesOneResultSlot) {
    service.set_mock_index({
        {LauncherResultKind::Application, "a", "Kitty Command", "", "", {}}
    });
    const auto results = service.search("kitty --single-instance", 1);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().kind, LauncherResultKind::Command);
}
