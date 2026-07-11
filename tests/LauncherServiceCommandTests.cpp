#include <filesystem>
#include <thread>
#include <gtest/gtest.h>
#include "services/LauncherService.hpp"

using namespace realmheart::services;

class RecordingCommandExecutor : public ILauncherCommandExecutor {
public:
    bool run_command(std::string_view command) override {
        commands.emplace_back(command);
        return next_result;
    }

    std::vector<std::string> commands;
    bool next_result = true;
};

class LauncherServiceCommandTest : public ::testing::Test {
protected:
    LauncherService service;
};

TEST_F(LauncherServiceCommandTest, CommandQueryReturnsCommandResult) {
    // Query starting with / should be treated as a command
    auto results = service.search("/usr/bin/kitty", 10);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].kind, LauncherResultKind::Command);
    EXPECT_EQ(results[0].id, "/usr/bin/kitty");
}

TEST_F(LauncherServiceCommandTest, SpaceInQueryReturnsCommandResult) {
    // Query with spaces should be treated as a command
    auto results = service.search("kitty -e htop", 10);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].kind, LauncherResultKind::Command);
    EXPECT_EQ(results[0].id, "kitty -e htop");
}

TEST_F(LauncherServiceCommandTest, ShellPrefixReturnsStrippedCommandResult) {
    auto results = service.search("$ btop", 10);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].kind, LauncherResultKind::Command);
    EXPECT_EQ(results[0].id, "btop");
}

TEST_F(LauncherServiceCommandTest, NormalQueryDoesNotForceCommand) {
    service.set_mock_index({{LauncherResultKind::Application, "org.gnome.Terminal", "Terminal", "GNOME Terminal", "utilities-terminal", {}}});
    auto results = service.search("Term", 10);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].kind, LauncherResultKind::Application);
}

TEST_F(LauncherServiceCommandTest, BareCommandIsAvailableWhenNoApplicationMatches) {
    service.set_mock_index({});
    auto results = service.search("btop", 10);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].kind, LauncherResultKind::Command);
    EXPECT_EQ(results[0].id, "btop");
}

TEST(LauncherServiceCommandActivationTest, ExecutesExactCommandThroughExecutor) {
    auto executor = std::make_unique<RecordingCommandExecutor>();
    auto* recorder = executor.get();
    LauncherService service(std::move(executor));
    LauncherResult command{
        LauncherResultKind::Command,
        "printf '%s' \"purple fire\" | wl-copy",
        "Execute Command",
        "",
        "utilities-terminal",
        {}
    };

    ASSERT_TRUE(service.activate(command));
    ASSERT_EQ(recorder->commands.size(), 1U);
    EXPECT_EQ(recorder->commands.front(), command.id);
}

TEST(LauncherServiceCommandActivationTest, RejectsBlankCommand) {
    auto executor = std::make_unique<RecordingCommandExecutor>();
    auto* recorder = executor.get();
    LauncherService service(std::move(executor));
    LauncherResult command{LauncherResultKind::Command, "  \t", "Execute Command", "", "", {}};

    EXPECT_FALSE(service.activate(command));
    EXPECT_TRUE(recorder->commands.empty());
}

TEST(LauncherServiceCommandTransportTest, NormalCommandRunsWithoutTerminal) {
    EXPECT_EQ(
        launcher_command_argv("npm run dev"),
        (std::vector<std::string>{"fish", "-C", "npm run dev"})
    );
}

TEST(LauncherServiceCommandLiveTest, HiddenCommandActuallyRuns) {
    SystemLauncherCommandExecutor executor;
    std::string probe = "touch /tmp/realmheart-live-ok";
    bool success = executor.run_command(probe);
    EXPECT_TRUE(success);
    
    bool found = false;
    for(int i=0; i<20; ++i) {
        if (std::filesystem::exists("/tmp/realmheart-live-ok")) {
            found = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(found);
    std::filesystem::remove("/tmp/realmheart-live-ok");
}
