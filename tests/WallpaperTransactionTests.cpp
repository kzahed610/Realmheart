#include "ui/wallpaper/WallpaperTransaction.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using realmheart::ui::wallpaper::WallpaperTransaction;

TEST(WallpaperTransactionTest, ReportsSuccessOnlyAfterPersistence) {
    std::vector<std::string> steps;
    int completion_calls = 0;
    bool result = false;
    std::string error;
    int persist_calls = 0;

    WallpaperTransaction::run({
        "/wallpapers/new.png",
        "/wallpapers/old.png",
        [&steps](const auto& path, auto callback) {
            steps.push_back("apply:" + path.string());
            callback(true, {});
        },
        [&steps](const auto& path, auto callback) {
            steps.push_back("rollback:" + path.string());
            callback(true, {});
        },
        [&steps, &persist_calls](const auto& path, std::string* error_message) {
            ++persist_calls;
            steps.push_back("persist:" + path.string());
            if (error_message != nullptr) error_message->clear();
            return true;
        },
        [&completion_calls, &result, &error](bool success, std::string message) {
            ++completion_calls;
            result = success;
            error = std::move(message);
        }
    });

    EXPECT_EQ(completion_calls, 1);
    EXPECT_TRUE(result);
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(persist_calls, 1);
    ASSERT_EQ(steps.size(), 2U);
    EXPECT_EQ(steps[0], "apply:/wallpapers/new.png");
    EXPECT_EQ(steps[1], "persist:/wallpapers/new.png");
}

TEST(WallpaperTransactionTest, PersistenceFailureRollsBackAndStillReportsFailure) {
    std::vector<std::string> steps;
    int completion_calls = 0;
    bool result = true;
    std::string error;

    WallpaperTransaction::run({
        "/wallpapers/new.png",
        "/wallpapers/old.png",
        [&steps](const auto&, auto callback) {
            steps.push_back("apply");
            callback(true, {});
        },
        [&steps](const auto& path, auto callback) {
            steps.push_back("rollback:" + path.string());
            callback(true, {});
        },
        [](const auto&, std::string* error_message) {
            if (error_message != nullptr) *error_message = "read-only state directory";
            return false;
        },
        [&completion_calls, &result, &error](bool success, std::string message) {
            ++completion_calls;
            result = success;
            error = std::move(message);
        }
    });

    EXPECT_EQ(completion_calls, 1);
    EXPECT_FALSE(result);
    EXPECT_NE(error.find("read-only state directory"), std::string::npos);
    EXPECT_NE(error.find("previous wallpaper restored"), std::string::npos);
    ASSERT_EQ(steps.size(), 2U);
    EXPECT_EQ(steps[0], "apply");
    EXPECT_EQ(steps[1], "rollback:/wallpapers/old.png");
}

TEST(WallpaperTransactionTest, PersistenceFailureWithoutPreviousStateIsExplicit) {
    int rollback_calls = 0;
    int completion_calls = 0;
    std::string error;

    WallpaperTransaction::run({
        "/wallpapers/new.png",
        std::nullopt,
        [](const auto&, auto callback) { callback(true, {}); },
        [&rollback_calls](const auto&, auto callback) {
            ++rollback_calls;
            callback(true, {});
        },
        [](const auto&, std::string*) { return false; },
        [&completion_calls, &error](bool success, std::string message) {
            ++completion_calls;
            EXPECT_FALSE(success);
            error = std::move(message);
        }
    });

    EXPECT_EQ(completion_calls, 1);
    EXPECT_EQ(rollback_calls, 0);
    EXPECT_NE(error.find("no previous wallpaper"), std::string::npos);
}

TEST(WallpaperTransactionTest, DuplicateVisualCallbacksAreExplicitlySuppressed) {
    int persist_calls = 0;
    int completion_calls = 0;

    WallpaperTransaction::run({
        "/wallpapers/new.png",
        std::nullopt,
        [](const auto&, auto callback) {
            callback(true, {});
            callback(false, "late stale callback");
        },
        {},
        [&persist_calls](const auto&, std::string*) {
            ++persist_calls;
            return true;
        },
        [&completion_calls](bool success, std::string) {
            ++completion_calls;
            EXPECT_TRUE(success);
        }
    });

    EXPECT_EQ(persist_calls, 1);
    EXPECT_EQ(completion_calls, 1);
}

} // namespace
