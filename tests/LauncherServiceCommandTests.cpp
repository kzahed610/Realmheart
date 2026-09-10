#include "core/Command.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#include <gtest/gtest.h>
#include "services/LauncherService.hpp"
#include "ui/launcher/CommandReceiptOverlay.hpp"

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

TEST_F(LauncherServiceCommandTest, ApplicationSearchKeepsRunCommandAsFinalChoice) {
    service.set_mock_index({{
        LauncherResultKind::Application,
        "kitty.desktop",
        "Kitty",
        "Terminal",
        "kitty",
        {}
    }});

    const auto results = service.search("kitty", 10);
    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].kind, LauncherResultKind::Application);
    EXPECT_EQ(results[0].id, "kitty.desktop");
    EXPECT_EQ(results[1].kind, LauncherResultKind::Command);
    EXPECT_EQ(results[1].id, "kitty");
}

TEST_F(LauncherServiceCommandTest, BareInstalledCommandIsAvailableWhenNoApplicationMatches) {
    service.set_mock_index({});
    auto results = service.search("sh", 10);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].kind, LauncherResultKind::Command);
    EXPECT_EQ(results[0].id, "sh");
}

TEST_F(LauncherServiceCommandTest, UnmatchedTextStillOffersRunCommand) {
    service.set_mock_index({});
    const auto results = service.search("definitely-not-a-realmheart-command", 10);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results.front().kind, LauncherResultKind::Command);
    EXPECT_EQ(results.front().id, "definitely-not-a-realmheart-command");
}

TEST_F(LauncherServiceCommandTest, ShellSyntaxIsAvailableWithoutExplicitMode) {
    service.set_mock_index({});
    const auto normal_results = service.search("printf hello | wl-copy", 10);
    ASSERT_EQ(normal_results.size(), 1U);
    EXPECT_EQ(normal_results.front().kind, LauncherResultKind::Command);

    const auto explicit_results = service.search("> printf hello | wl-copy", 10);
    ASSERT_EQ(explicit_results.size(), 1U);
    EXPECT_EQ(explicit_results[0].kind, LauncherResultKind::Command);
}

TEST_F(LauncherServiceCommandTest, ArithmeticOffersCalculationBeforeRunCommand) {
    service.set_mock_index({});
    const auto results = service.search("24 * 18", 10);

    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].kind, LauncherResultKind::Calculation);
    EXPECT_EQ(results[0].title, "Calculate");
    EXPECT_EQ(results[0].subtitle, "24 × 18 = 432");
    EXPECT_EQ(results[0].id, "432");
    EXPECT_EQ(results[1].kind, LauncherResultKind::Command);
    EXPECT_EQ(results[1].id, "24 * 18");
}

TEST_F(LauncherServiceCommandTest, CalculatorHonoursPrecedenceAndParentheses) {
    service.set_mock_index({});

    const auto precedence = service.search("2 + 3 * 4", 10);
    ASSERT_GE(precedence.size(), 1U);
    EXPECT_EQ(precedence[0].kind, LauncherResultKind::Calculation);
    EXPECT_EQ(precedence[0].id, "14");

    const auto parentheses = service.search("(2 + 3) * 4", 10);
    ASSERT_GE(parentheses.size(), 1U);
    EXPECT_EQ(parentheses[0].kind, LauncherResultKind::Calculation);
    EXPECT_EQ(parentheses[0].id, "20");
}

TEST_F(LauncherServiceCommandTest, CalculatorSupportsPowerModuloAndUnarySigns) {
    service.set_mock_index({});

    EXPECT_EQ(service.search("2 ^ 3 ^ 2", 10).front().id, "512");
    EXPECT_EQ(service.search("17 % 5", 10).front().id, "2");
    EXPECT_EQ(service.search("-2 ^ 2", 10).front().id, "-4");
    EXPECT_EQ(service.search("2 ^ -2", 10).front().id, "0.25");
}

TEST_F(LauncherServiceCommandTest, InvalidArithmeticFallsBackToRunCommandOnly) {
    service.set_mock_index({});

    const auto malformed = service.search("2 +", 10);
    ASSERT_EQ(malformed.size(), 1U);
    EXPECT_EQ(malformed[0].kind, LauncherResultKind::Command);

    const auto division_by_zero = service.search("9 / 0", 10);
    ASSERT_EQ(division_by_zero.size(), 1U);
    EXPECT_EQ(division_by_zero[0].kind, LauncherResultKind::Command);

    const auto bare_number = service.search("42", 10);
    ASSERT_EQ(bare_number.size(), 1U);
    EXPECT_EQ(bare_number[0].kind, LauncherResultKind::Command);
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

TEST(LauncherServiceCalculationActivationTest, CopiesResultThroughProcessExecutor) {
    auto command_executor = std::make_unique<RecordingCommandExecutor>();
    auto process_executor = std::make_unique<RecordingProcessExecutor>();
    auto* recorder = process_executor.get();
    LauncherService service(std::move(command_executor), std::move(process_executor));

    LauncherResult calculation{
        LauncherResultKind::Calculation,
        "432",
        "Calculate",
        "24 × 18 = 432",
        "accessories-calculator",
        {}
    };

    ASSERT_TRUE(service.activate(calculation));
    ASSERT_EQ(recorder->commands.size(), 1U);
    EXPECT_EQ(
        recorder->commands.front(),
        (std::vector<std::string>{"wl-copy", "432"})
    );
}

TEST(LauncherServiceCommandTransportTest, NormalCommandRunsWithoutTerminal) {
    EXPECT_EQ(
        launcher_command_argv("npm run dev"),
        (std::vector<std::string>{"fish", "-C", "npm run dev"})
    );
}

TEST(LauncherCommandBoundaryTest, CapturesStdoutAndStderrWithinSeparateBounds) {
    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::seconds(2);
    options.max_output_bytes = 32;
    options.separate_stderr = true;

    const auto result = realmheart::core::run_capture(
        {"sh", "-c", "printf stdout; printf stderr >&2"},
        options
    );

    ASSERT_TRUE(result.succeeded()) << result.error;
    EXPECT_EQ(result.output, "stdout");
    EXPECT_EQ(result.standard_error, "stderr");
    EXPECT_FALSE(result.truncated);
}

TEST(LauncherCommandBoundaryTest, TerminatesNeverEndingOutputAtDeadlineWithoutUnboundedCapture) {
    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::milliseconds(120);
    options.terminate_grace = std::chrono::milliseconds(40);
    options.max_output_bytes = 4096;
    options.separate_stderr = true;

    const auto started = std::chrono::steady_clock::now();
    const auto result = realmheart::core::run_capture(
        {"sh", "-c", "while :; do printf x; printf y >&2; done"},
        options
    );
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(result.status, realmheart::core::CommandStatus::TimedOut);
    EXPECT_LE(result.output.size(), options.max_output_bytes);
    EXPECT_LE(result.standard_error.size(), options.max_output_bytes);
    EXPECT_TRUE(result.truncated);
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(LauncherCommandBoundaryTest, CancellationTerminatesAndReapsDescendants) {
    std::atomic_bool cancelled = false;
    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::seconds(10);
    options.terminate_grace = std::chrono::milliseconds(40);
    options.cancelled = [&cancelled] { return cancelled.load(); };

    std::thread canceller([&cancelled] {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        cancelled.store(true);
    });
    const auto result = realmheart::core::run_capture(
        {"sh", "-c", "sleep 60 & wait"},
        options
    );
    canceller.join();

    EXPECT_EQ(result.status, realmheart::core::CommandStatus::Cancelled);
    EXPECT_TRUE(result.error.find("cancelled") != std::string::npos);
}

TEST(LauncherCommandReceiptContractTest, PropagatesAndDisplaysTruncationMetadata) {
    const auto source_root = std::filesystem::path(REALMHEART_SOURCE_DIR);
    const auto source_path = source_root /
        "src/ui/launcher/CommandReceiptOverlay.cpp";
    const auto header_path = source_root /
        "src/ui/launcher/CommandReceiptOverlay.hpp";
    std::ifstream source(source_path);
    std::ifstream header(header_path);
    ASSERT_TRUE(source.good()) << source_path;
    ASSERT_TRUE(header.good()) << header_path;
    const std::string contents(
        (std::istreambuf_iterator<char>(source)),
        std::istreambuf_iterator<char>()
    );
    const std::string header_contents(
        (std::istreambuf_iterator<char>(header)),
        std::istreambuf_iterator<char>()
    );

    EXPECT_NE(contents.find("result.truncated"), std::string::npos);
    EXPECT_NE(contents.find("output_truncated"), std::string::npos);
    EXPECT_NE(
        header_contents.find("Realmheart truncated command output"),
        std::string::npos
    );
}

TEST(LauncherCommandReceiptContractTest, MarksOnlyTruncatedOutput) {
    using realmheart::ui::command_receipt_detail::annotate_output_truncation;
    using realmheart::ui::command_receipt_detail::kOutputTruncationMarker;

    EXPECT_EQ(annotate_output_truncation("captured", false), "captured");
    EXPECT_EQ(
        annotate_output_truncation("captured", true),
        "captured\n\n" + std::string(kOutputTruncationMarker)
    );
    EXPECT_EQ(
        annotate_output_truncation(
            std::string(kOutputTruncationMarker),
            true
        ),
        kOutputTruncationMarker
    );
}

TEST(ShellShutdownContractTest, DetachesCommandReceiptsBeforeExecutorDrain) {
    const auto path = std::filesystem::path(REALMHEART_SOURCE_DIR) /
        "src/ui/ShellApp.cpp";
    std::ifstream source(path);
    ASSERT_TRUE(source.good()) << path;
    const std::string contents(
        (std::istreambuf_iterator<char>(source)),
        std::istreambuf_iterator<char>()
    );

    const auto detach = contents.find("command_receipts_->detach()");
    const auto drain = contents.find(
        "core::shared_task_executor().wait_for_idle()"
    );
    ASSERT_NE(detach, std::string::npos);
    ASSERT_NE(drain, std::string::npos);
    EXPECT_LT(detach, drain);
}

TEST(LauncherEmojiInputBoundaryTest, RejectsInputThatExceedsLimitDuringRead) {
    const auto path = std::filesystem::temp_directory_path() /
        "realmheart-task20-emoji-oversize.txt";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << "### DATA ###\n" << std::string(129, 'x');
    }

    const auto contents = realmheart::services::read_bounded_emoji_data(path, 128);
    std::error_code error;
    std::filesystem::remove(path, error);
    EXPECT_FALSE(contents.has_value());
}

TEST(LauncherEmojiInputBoundaryTest, AcceptsInputExactlyAtReadLimit) {
    const auto path = std::filesystem::temp_directory_path() /
        "realmheart-task20-emoji-exact-limit.txt";
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << std::string(128, 'x');
    }

    const auto contents = realmheart::services::read_bounded_emoji_data(path, 128);
    std::error_code error;
    std::filesystem::remove(path, error);
    ASSERT_TRUE(contents.has_value());
    EXPECT_EQ(contents->size(), 128U);
}

TEST(LauncherServiceCommandLiveTest, HiddenCommandActuallyRuns) {
    char directory_template[] = "/tmp/realmheart-launcher-command-XXXXXX";
    char* directory = ::mkdtemp(directory_template);
    ASSERT_NE(directory, nullptr);
    const auto probe_directory = std::filesystem::path(directory);
    const auto marker = probe_directory / "marker";

    SystemLauncherCommandExecutor executor;
    const std::string probe = "printf '%s' launcher-ok > " + marker.string();
    bool success = executor.run_command(probe);
    EXPECT_TRUE(success);

    bool found = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        std::ifstream input(marker);
        const std::string contents{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
        if (contents == "launcher-ok") {
            found = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    EXPECT_TRUE(found);
    std::error_code error;
    std::filesystem::remove_all(probe_directory, error);
    EXPECT_FALSE(error);
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

TEST(LauncherClipboardParsingTest, ParsesTextAndImageEntriesWithoutBinaryGarbage) {
    const std::string history =
        "105\tgit apply --check launcher.patch\n"
        "104\t[[ binary data 1.4 MiB png 1920x1080 ]]\n"
        "103\t[[ binary data 40 KiB application/octet-stream ]]\n"
        "102\tA second text entry\n";

    const auto results = launcher_clipboard_results(history, "", 10);
    ASSERT_EQ(results.size(), 4U);

    EXPECT_EQ(results[0].kind, LauncherResultKind::Clipboard);
    EXPECT_EQ(results[0].id, "105");
    EXPECT_EQ(results[0].title, "git apply --check launcher.patch");
    EXPECT_FALSE(results[0].clipboard_image);

    EXPECT_EQ(results[1].id, "104");
    EXPECT_TRUE(results[1].clipboard_image);
    EXPECT_EQ(results[1].clipboard_mime, "image/png");
    EXPECT_EQ(results[1].title, "Screenshot or copied image");
    EXPECT_EQ(results[1].icon_name, "Realmheart-Icons/clip-history.svg");

    EXPECT_EQ(results[2].id, "103");
    EXPECT_FALSE(results[2].clipboard_image);
    EXPECT_EQ(results[2].title, "Binary clipboard entry");
}

TEST(LauncherClipboardParsingTest, FiltersTextAndImageAliasesAndHonoursLimit) {
    const std::string history =
        "9\t[[ binary data 900 KiB jpeg 900x600 ]]\n"
        "8\tRealmheart clipboard integration\n"
        "7\tUnrelated text\n";

    const auto screenshot = launcher_clipboard_results(history, "screenshot", 10);
    ASSERT_EQ(screenshot.size(), 1U);
    EXPECT_EQ(screenshot[0].id, "9");

    const auto realmheart = launcher_clipboard_results(history, "realmheart", 10);
    ASSERT_EQ(realmheart.size(), 1U);
    EXPECT_EQ(realmheart[0].id, "8");

    const auto limited = launcher_clipboard_results(history, "", 2);
    ASSERT_EQ(limited.size(), 2U);
    EXPECT_EQ(limited[0].id, "9");
    EXPECT_EQ(limited[1].id, "8");
}

TEST(LauncherClipboardParsingTest, ClearHistoryRequiresAVisibleConfirmationState) {
    const auto normal = launcher_clipboard_clear_result(false);
    EXPECT_EQ(normal.kind, LauncherResultKind::ClipboardAction);
    EXPECT_EQ(normal.id, "clear-history");
    EXPECT_EQ(normal.title, "Clear clipboard history");

    const auto armed = launcher_clipboard_clear_result(true);
    EXPECT_EQ(armed.kind, LauncherResultKind::ClipboardAction);
    EXPECT_NE(armed.title.find("Enter again"), std::string::npos);
}

TEST(LauncherServiceClipboardActivationTest, RestoresTextThroughCliphistAndWlCopy) {
    auto command_executor = std::make_unique<RecordingCommandExecutor>();
    auto process_executor = std::make_unique<RecordingProcessExecutor>();
    auto* recorder = process_executor.get();
    LauncherService service(std::move(command_executor), std::move(process_executor));

    LauncherResult entry;
    entry.kind = LauncherResultKind::Clipboard;
    entry.id = "105";

    ASSERT_TRUE(service.activate(entry));
    ASSERT_EQ(recorder->commands.size(), 1U);
    EXPECT_EQ(
        recorder->commands.front(),
        (std::vector<std::string>{
            "sh",
            "-c",
            "cliphist decode \"$1\" | wl-copy",
            "realmheart-clipboard",
            "105",
        })
    );
}

TEST(LauncherServiceClipboardActivationTest, RestoresImagesWithTheirMimeType) {
    auto command_executor = std::make_unique<RecordingCommandExecutor>();
    auto process_executor = std::make_unique<RecordingProcessExecutor>();
    auto* recorder = process_executor.get();
    LauncherService service(std::move(command_executor), std::move(process_executor));

    LauncherResult entry;
    entry.kind = LauncherResultKind::Clipboard;
    entry.id = "104";
    entry.clipboard_image = true;
    entry.clipboard_mime = "image/png";

    ASSERT_TRUE(service.activate(entry));
    ASSERT_EQ(recorder->commands.size(), 1U);
    EXPECT_EQ(
        recorder->commands.front(),
        (std::vector<std::string>{
            "sh",
            "-c",
            "cliphist decode \"$1\" | wl-copy --type \"$2\"",
            "realmheart-clipboard",
            "104",
            "image/png",
        })
    );
}

TEST(LauncherCommandSuggestionTest, ListsAvailableCommandsFromChevronPrefix) {
    const auto results = launcher_command_suggestions(">");
    ASSERT_EQ(results.size(), 3U);
    EXPECT_EQ(results[0].kind, LauncherResultKind::LauncherCommand);
    EXPECT_EQ(results[0].id, "clip");
    EXPECT_EQ(results[0].title, ">clip");
    EXPECT_EQ(results[1].id, "clear");
    EXPECT_EQ(results[1].title, ">clear");
    EXPECT_EQ(results[2].id, "emoji");
    EXPECT_EQ(results[2].title, ">emoji");
}

TEST(LauncherCommandSuggestionTest, FiltersSuggestionsByTypedPrefix) {
    const auto clip = launcher_command_suggestions(">cli");
    ASSERT_EQ(clip.size(), 1U);
    EXPECT_EQ(clip[0].id, "clip");

    const auto clear = launcher_command_suggestions("  >cle  ");
    ASSERT_EQ(clear.size(), 1U);
    EXPECT_EQ(clear[0].id, "clear");

    EXPECT_TRUE(launcher_command_suggestions(">unknown").empty());
    EXPECT_TRUE(launcher_command_suggestions(">clip filter").empty());
    EXPECT_TRUE(launcher_command_suggestions("clip").empty());
}
