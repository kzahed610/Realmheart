#include <gtest/gtest.h>

#include "services/HyprlandSession.hpp"

using namespace realmheart::services;

TEST(HyprlandSessionTest, ParsesMappedClientsAndActiveWindow) {
    const auto snapshot = HyprlandSession::parse(
        R"json([
          {
            "address": "0xabc",
            "mapped": true,
            "class": "zen",
            "title": "Launcher design",
            "workspace": {"id": 2},
            "focusHistoryID": 0
          },
          {
            "address": "0xdef",
            "mapped": true,
            "class": "kitty",
            "title": "Realmheart build",
            "workspace": {"id": 1},
            "focusHistoryID": 1
          }
        ])json",
        R"json({"address":"0xabc"})json"
    );

    ASSERT_TRUE(snapshot.available);
    ASSERT_EQ(snapshot.windows.size(), 2U);
    EXPECT_EQ(snapshot.windows[0].app_id, "zen");
    EXPECT_EQ(snapshot.windows[0].title, "Launcher design");
    EXPECT_EQ(snapshot.windows[0].workspace_id, 2);
    EXPECT_TRUE(snapshot.windows[0].active);
    EXPECT_EQ(snapshot.windows[1].app_id, "kitty");
    EXPECT_FALSE(snapshot.windows[1].active);
}

TEST(HyprlandSessionTest, SkipsUnmappedAndMalformedClients) {
    const auto snapshot = HyprlandSession::parse(
        R"json([
          {"address":"0xabc","mapped":false,"class":"hidden"},
          {"address":"","mapped":true,"class":"missing-address"},
          {"address":"0xdef","mapped":true,"class":""},
          {"address":"0x123","mapped":true,"initialClass":"dolphin","initialTitle":"Files"}
        ])json"
    );

    ASSERT_TRUE(snapshot.available);
    ASSERT_EQ(snapshot.windows.size(), 1U);
    EXPECT_EQ(snapshot.windows.front().app_id, "dolphin");
    EXPECT_EQ(snapshot.windows.front().title, "Files");
}

TEST(HyprlandSessionTest, RejectsInvalidJson) {
    const auto snapshot = HyprlandSession::parse("not-json");
    EXPECT_FALSE(snapshot.available);
    EXPECT_TRUE(snapshot.windows.empty());
}
