#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unistd.h>

#include "services/HyprlandSession.hpp"

using namespace realmheart::services;


namespace {

class TemporaryHyprctl {
public:
    explicit TemporaryHyprctl(std::string_view script_body) {
        root_ = std::filesystem::temp_directory_path() /
            ("realmheart-hyprland-session-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
        output_ = root_ / "arguments.txt";

        const auto executable = root_ / "hyprctl";
        std::ofstream script(executable);
        script << "#!/bin/sh\n" << script_body;
        script.close();
        std::filesystem::permissions(
            executable,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write |
                std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace
        );

        const char* old_path = std::getenv("PATH");
        old_path_ = old_path == nullptr ? std::string{} : std::string(old_path);
        const std::string path = root_.string() +
            (old_path_.empty() ? std::string{} : ":" + old_path_);
        ::setenv("PATH", path.c_str(), 1);
        ::setenv("REALMHEART_HYPRCTL_TEST_OUTPUT", output_.c_str(), 1);
    }

    ~TemporaryHyprctl() {
        ::setenv("PATH", old_path_.c_str(), 1);
        ::unsetenv("REALMHEART_HYPRCTL_TEST_OUTPUT");
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] std::string output() const {
        std::ifstream recorded(output_);
        return {
            std::istreambuf_iterator<char>(recorded),
            std::istreambuf_iterator<char>()
        };
    }

private:
    std::filesystem::path root_;
    std::filesystem::path output_;
    std::string old_path_;
};

} // namespace

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

TEST(HyprlandSessionTest, RejectsInvalidWorkspaceMoveRequests) {
    EXPECT_FALSE(HyprlandSession::move_window_to_workspace("", 2));
    EXPECT_FALSE(HyprlandSession::move_window_to_workspace("not-an-address", 2));
    EXPECT_FALSE(HyprlandSession::move_window_to_workspace("0xabc", 0));
}


TEST(HyprlandSessionTest, MovesExactWindowWithLuaDispatcher) {
    TemporaryHyprctl hyprctl(
        "printf '%s\\n' \"$*\" > \"$REALMHEART_HYPRCTL_TEST_OUTPUT\"\n"
        "printf '%s\\n' 'ok'\n"
    );

    EXPECT_TRUE(HyprlandSession::move_window_to_workspace("ABC", 4));
    EXPECT_EQ(
        hyprctl.output(),
        "dispatch hl.dsp.window.move({ workspace = 4, follow = false, "
        "window = \"address:0xabc\" })\n"
    );
}

TEST(HyprlandSessionTest, FallsBackToLegacyMoveDispatcher) {
    TemporaryHyprctl hyprctl(
        "printf '%s\\n' \"$*\" >> \"$REALMHEART_HYPRCTL_TEST_OUTPUT\"\n"
        "case \"$2\" in\n"
        "  hl.dsp.window.move*) printf '%s\\n' 'error: unsupported dispatcher' ;;\n"
        "  *) printf '%s\\n' 'ok' ;;\n"
        "esac\n"
    );

    EXPECT_TRUE(HyprlandSession::move_window_to_workspace("0xabc", 2));
    EXPECT_EQ(
        hyprctl.output(),
        "dispatch hl.dsp.window.move({ workspace = 2, follow = false, "
        "window = \"address:0xabc\" })\n"
        "dispatch movetoworkspacesilent 2,address:0xabc\n"
    );
}
