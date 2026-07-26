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
    const auto results = service.search("> kitty --single-instance", 1);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().kind, LauncherResultKind::Command);
}

TEST_F(LauncherServiceTest, RecommendationsContainOnlyLaunchableEntriesAndRespectLimit) {
    service.set_mock_index({
        {LauncherResultKind::Emoji, "emoji", "Fire", "", "", {}},
        {LauncherResultKind::Action, "action", "Build", "", "", {}},
        {LauncherResultKind::Application, "z", "Zen", "", "", {}},
        {LauncherResultKind::Application, "a", "Antigravity", "", "", {}}
    });

    const auto results = service.recommendations(2);
    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].id, "a");
    EXPECT_EQ(results[1].id, "z");
}

TEST_F(LauncherServiceTest, SearchesExecutableAndDescriptionMetadata) {
    LauncherResult browser{
        LauncherResultKind::Application,
        "org.example.Browser.desktop",
        "Aurora",
        "",
        "web-browser",
        {}
    };
    browser.description = "Private web browser";
    browser.executable = "aurora-browser";
    browser.search_terms = {"internet", "navigation"};
    service.set_mock_index({browser});

    ASSERT_EQ(service.search("aurora-browser", 10).front().id, browser.id);
    ASSERT_EQ(service.search("private web", 10).front().id, browser.id);
    ASSERT_EQ(service.search("navigation", 10).front().id, browser.id);
}

TEST_F(LauncherServiceTest, AcronymSearchFindsApplication) {
    service.set_mock_index({
        {LauncherResultKind::Application, "code", "Visual Studio Code", "", "", {}}
    });

    const auto results = service.search("vsc", 10);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results.front().id, "code");
}

TEST_F(LauncherServiceTest, FuzzySearchFindsApplication) {
    service.set_mock_index({
        {LauncherResultKind::Application, "gravity", "Antigravity", "", "", {}}
    });

    const auto results = service.search("antgrvty", 10);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results.front().id, "gravity");
}

TEST(LauncherServiceHelpersTest, ParsesAndFiltersEmbeddedEmojiData) {
    constexpr std::string_view script = R"EMOJI(#!/usr/bin/env bash
emoji="$(sed '1,/^### DATA ###$/d' "$0" | fuzzel --dmenu)"
case "$MODE" in
    copy) wl-copy "$emoji" ;;
esac
### DATA ###
😭 loudly crying face cry tears sad
⚔️ crossed swords weapon battle
📋 clipboard stationery documents
)EMOJI";

    const auto all = launcher_emoji_results(script, "", 10);
    ASSERT_EQ(all.size(), 3U);
    EXPECT_EQ(all.front().kind, LauncherResultKind::Emoji);
    EXPECT_EQ(all.front().id, "😭");
    EXPECT_EQ(all.front().icon_name, "Realmheart-Icons/emoji-picker.svg");

    const auto filtered = launcher_emoji_results(script, "cry tears", 10);
    ASSERT_EQ(filtered.size(), 1U);
    EXPECT_EQ(filtered.front().id, "😭");

    const auto limited = launcher_emoji_results(script, "", 2);
    EXPECT_EQ(limited.size(), 2U);
}

TEST(LauncherServiceHelpersTest, SuggestsEmojiLauncherCommand) {
    const auto results = launcher_command_suggestions(">em");
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().id, "emoji");
    EXPECT_EQ(results.front().icon_name, "Realmheart-Icons/emoji-picker.svg");
}

TEST(LauncherServiceHelpersTest, BuildsExactClipboardDeleteCommand) {
    const auto argv = launcher_clipboard_delete_argv("42", "copied text");
    ASSERT_EQ(argv.size(), 6U);
    EXPECT_EQ(argv[0], "sh");
    EXPECT_EQ(argv[1], "-c");
    EXPECT_EQ(argv[2], "printf '%s\\t%s\\n' \"$1\" \"$2\" | cliphist delete");
    EXPECT_EQ(argv[3], "realmheart-clipboard-delete");
    EXPECT_EQ(argv[4], "42");
    EXPECT_EQ(argv[5], "copied text");
}
