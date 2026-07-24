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


class RecordingProcessExecutor : public ILauncherProcessExecutor {
public:
    bool run(const std::vector<std::string>& argv) override {
        commands.push_back(argv);
        return next_result;
    }

    std::vector<std::vector<std::string>> commands;
    bool next_result = true;
};

class LauncherServiceCommandTest : public ::testing::Test {
protected:
    LauncherService service;
};

TEST_F(LauncherServiceCommandTest, ExplicitPrefixReturnsCommandResult) {
    auto results = service.search("> /usr/bin/kitty", 10);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].kind, LauncherResultKind::Command);
    EXPECT_EQ(results[0].id, "/usr/bin/kitty");
}

TEST_F(LauncherServiceCommandTest, ValidExecutableWithArgumentsReturnsCommandCandidate) {
    auto results = service.search("sh -c true", 10);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results.back().kind, LauncherResultKind::Command);
    EXPECT_EQ(results.back().id, "sh -c true");
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

TEST_F(LauncherServiceCommandTest, BareInstalledCommandIsAvailableWhenNoApplicationMatches) {
    service.set_mock_index({});
    auto results = service.search("sh", 10);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].kind, LauncherResultKind::Command);
    EXPECT_EQ(results[0].id, "sh");
}

TEST_F(LauncherServiceCommandTest, UnmatchedTextDoesNotBecomeACommand) {
    service.set_mock_index({});
    auto results = service.search("definitely-not-a-realmheart-command", 10);
    EXPECT_TRUE(results.empty());
}

TEST_F(LauncherServiceCommandTest, ShellSyntaxRequiresExplicitMode) {
    service.set_mock_index({});
    EXPECT_TRUE(service.search("printf hello | wl-copy", 10).empty());

    const auto explicit_results = service.search("> printf hello | wl-copy", 10);
    ASSERT_EQ(explicit_results.size(), 1U);
    EXPECT_EQ(explicit_results[0].kind, LauncherResultKind::Command);
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

TEST(LauncherServiceScopeTest, WrapsArgvInIndependentUserScope) {
    EXPECT_EQ(
        launcher_scoped_argv({"code", "--new-window"}),
        (std::vector<std::string>{
            "systemd-run", "--user", "--scope", "--quiet", "--collect",
            "--slice=app.slice", "--", "code", "--new-window"
        })
    );
}

TEST(LauncherServiceScopeTest, ApplicationUsesGtk4LaunchInsideProcessExecutor) {
    auto command_executor = std::make_unique<RecordingCommandExecutor>();
    auto process_executor = std::make_unique<RecordingProcessExecutor>();
    auto* recorder = process_executor.get();
    LauncherService service(std::move(command_executor), std::move(process_executor));

    LauncherResult application{
        LauncherResultKind::Application,
        "antigravity.desktop",
        "Antigravity",
        "",
        "",
        {}
    };

    ASSERT_TRUE(service.activate(application));
    ASSERT_EQ(recorder->commands.size(), 1U);
    EXPECT_EQ(
        recorder->commands.front(),
        (std::vector<std::string>{"gtk4-launch", "antigravity.desktop"})
    );
}

TEST(LauncherServiceScopeTest, ActionUsesIndependentProcessExecutor) {
    auto command_executor = std::make_unique<RecordingCommandExecutor>();
    auto process_executor = std::make_unique<RecordingProcessExecutor>();
    auto* recorder = process_executor.get();
    LauncherService service(std::move(command_executor), std::move(process_executor));

    LauncherResult action{
        LauncherResultKind::Action,
        "/home/test/.config/realmheart/actions/build.sh",
        "Build",
        "",
        "",
        {}
    };

    ASSERT_TRUE(service.activate(action));
    ASSERT_EQ(recorder->commands.size(), 1U);
    EXPECT_EQ(
        recorder->commands.front(),
        (std::vector<std::string>{"/bin/bash", action.id})
    );
}
