#include "wallpaper-native/NativeWallpaperContracts.hpp"
#include "ui/wallpaper/NativeWallpaperBackend.hpp"

#include <gtest/gtest.h>
#include <glib.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <unistd.h>
#include <vector>

#ifndef REALMHEART_TEST_NATIVE_WALLPAPER_RENDERER
#error "The deterministic native wallpaper test renderer must be configured"
#endif

namespace {

using realmheart::wallpaper_native::NativeRecoveryState;
using realmheart::wallpaper_native::NativeBinaryCommand;
using realmheart::wallpaper_native::NativeBinaryHeader;
using realmheart::wallpaper_native::NativeWallpaperInput;
using realmheart::wallpaper_native::NativeWallpaperProtocolDecoder;
using realmheart::wallpaper_native::NativeRestartDecision;
using realmheart::wallpaper_native::encode_native_binary_header;
using realmheart::wallpaper_native::native_command_ready;
using realmheart::wallpaper_native::native_output_requires_redecode;
using realmheart::wallpaper_native::native_output_should_recreate;
using realmheart::wallpaper_native::native_restart_decision;
using realmheart::wallpaper_native::kNativeMaxCommandLineBytes;
using realmheart::wallpaper_native::kNativeMaxSourceBytes;
using realmheart::wallpaper_native::parse_native_binary_header;

using realmheart::services::WallpaperSource;
using realmheart::ui::wallpaper::NativeWallpaperBackend;
using realmheart::ui::wallpaper::WallpaperOutputTarget;

std::vector<std::string> read_records(const std::filesystem::path& path) {
    std::vector<std::string> records;
    std::ifstream input(path);
    std::string record;
    while (std::getline(input, record)) records.push_back(std::move(record));
    return records;
}

class RendererEnvironment final {
public:
    RendererEnvironment() {
        static unsigned sequence = 0;
        const std::string prefix =
            "realmheart-native-wallpaper-test-" +
            std::to_string(static_cast<unsigned long>(::getpid())) + "-" +
            std::to_string(sequence++);
        log_path_ = std::filesystem::temp_directory_path() / (prefix + ".log");
        state_path_ = std::filesystem::temp_directory_path() / (prefix + ".state");
        std::error_code ignored;
        std::filesystem::remove(log_path_, ignored);
        std::filesystem::remove(state_path_, ignored);
        g_setenv(
            "REALMHEART_WALLPAPER_RENDERER",
            REALMHEART_TEST_NATIVE_WALLPAPER_RENDERER,
            TRUE
        );
        g_setenv("REALMHEART_TEST_RENDERER_LOG", log_path_.c_str(), TRUE);
        g_setenv("REALMHEART_TEST_RENDERER_STATE", state_path_.c_str(), TRUE);
    }

    ~RendererEnvironment() {
        g_unsetenv("REALMHEART_WALLPAPER_RENDERER");
        g_unsetenv("REALMHEART_TEST_RENDERER_LOG");
        g_unsetenv("REALMHEART_TEST_RENDERER_STATE");
        g_unsetenv("REALMHEART_TEST_RENDERER_EXIT_AFTER_SET");
        g_unsetenv("REALMHEART_TEST_RENDERER_EXIT_AFTER_COMMIT");
        std::error_code ignored;
        std::filesystem::remove(log_path_, ignored);
        std::filesystem::remove(state_path_, ignored);
    }

    void exit_after_set() {
        g_setenv("REALMHEART_TEST_RENDERER_EXIT_AFTER_SET", "1", TRUE);
    }

    void exit_after_commit() {
        g_setenv("REALMHEART_TEST_RENDERER_EXIT_AFTER_COMMIT", "1", TRUE);
    }

    [[nodiscard]] const std::filesystem::path& log_path() const noexcept {
        return log_path_;
    }

private:
    std::filesystem::path log_path_;
    std::filesystem::path state_path_;
};

bool wait_for_records(const std::filesystem::path& path, std::size_t count) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        while (g_main_context_pending(nullptr)) {
            g_main_context_iteration(nullptr, false);
        }
        if (read_records(path).size() >= count) return true;
        g_usleep(2'000);
    }
    return read_records(path).size() >= count;
}

TEST(NativeWallpaperLifecycleTest, RecoveryIsBoundedAndRequiresReplayState) {
    EXPECT_EQ(
        native_restart_decision({false, true, 0}),
        NativeRestartDecision::RestartAndReplay
    );
    EXPECT_EQ(
        native_restart_decision({false, true, 1}),
        NativeRestartDecision::FailClosed
    );
    EXPECT_EQ(
        native_restart_decision({false, false, 0}),
        NativeRestartDecision::FailClosed
    );
    EXPECT_EQ(
        native_restart_decision({true, true, 0}),
        NativeRestartDecision::Suppress
    );
}

TEST(NativeWallpaperLifecycleTest, CommandCannotSucceedWithoutEveryReadinessBoundary) {
    EXPECT_TRUE(native_command_ready(true, true, true));
    EXPECT_FALSE(native_command_ready(false, true, true));
    EXPECT_FALSE(native_command_ready(true, false, true));
    EXPECT_FALSE(native_command_ready(true, true, false));
}

TEST(NativeWallpaperLifecycleTest, ClosedOutputOnlyRecreatesWhenWaylandOutputRemains) {
    EXPECT_TRUE(native_output_should_recreate(true, true));
    EXPECT_FALSE(native_output_should_recreate(false, true));
    EXPECT_FALSE(native_output_should_recreate(true, false));
}

TEST(NativeWallpaperLifecycleTest, TopologyRedeocodeOnlyRunsForLargerRequiredPixels) {
    EXPECT_TRUE(native_output_requires_redecode(1920, 1080, 3840, 2160));
    EXPECT_TRUE(native_output_requires_redecode(3840, 2160, 3840, 2161));
    EXPECT_FALSE(native_output_requires_redecode(3840, 2160, 2560, 1440));
}

TEST(NativeWallpaperProtocolTest, EncodesAndDecodesPartialBinaryFrames) {
    const std::string payload("\0owned-pixels", 13);
    std::string error;
    const auto encoded = encode_native_binary_header(
        NativeBinaryCommand::PrepareOutput, "RFAtMQ==", payload.size(), &error
    );
    ASSERT_TRUE(encoded.has_value()) << error;

    NativeWallpaperProtocolDecoder decoder;
    std::vector<NativeWallpaperInput> inputs;
    ASSERT_TRUE(decoder.feed(encoded->substr(0, 5), &inputs, &error)) << error;
    EXPECT_TRUE(inputs.empty());
    ASSERT_TRUE(decoder.feed(encoded->substr(5) + payload.substr(0, 4), &inputs, &error))
        << error;
    EXPECT_TRUE(inputs.empty());
    ASSERT_TRUE(decoder.feed(payload.substr(4), &inputs, &error)) << error;
    ASSERT_EQ(inputs.size(), 1U);
    EXPECT_EQ(inputs[0].kind, NativeWallpaperInput::Kind::BinaryFrame);
    EXPECT_EQ(inputs[0].binary_frame.command, NativeBinaryCommand::PrepareOutput);
    EXPECT_EQ(inputs[0].binary_frame.encoded_output_token, "RFAtMQ==");
    EXPECT_EQ(inputs[0].binary_frame.payload, payload);
    EXPECT_TRUE(decoder.finish(&error)) << error;
}

TEST(NativeWallpaperProtocolTest, PreservesLegacyAndZeroLengthFrames) {
    NativeWallpaperProtocolDecoder decoder;
    std::vector<NativeWallpaperInput> inputs;
    std::string error;
    ASSERT_TRUE(decoder.feed("SET legacy-path\nSET_BYTES 0\n", &inputs, &error)) << error;
    ASSERT_EQ(inputs.size(), 2U);
    EXPECT_EQ(inputs[0].kind, NativeWallpaperInput::Kind::LegacyCommand);
    EXPECT_EQ(inputs[0].legacy_command, "SET legacy-path");
    EXPECT_EQ(inputs[1].kind, NativeWallpaperInput::Kind::BinaryFrame);
    EXPECT_EQ(inputs[1].binary_frame.command, NativeBinaryCommand::Set);
    EXPECT_TRUE(inputs[1].binary_frame.payload.empty());
    EXPECT_TRUE(decoder.finish(&error)) << error;
}

TEST(NativeWallpaperProtocolTest, RejectsMalformedHeadersBeforePayloadAllocation) {
    const std::vector<std::string> malformed{
        "SET_BYTES",
        "SET_BYTES -1",
        "SET_BYTES 1 trailing",
        "PREPARE_OUTPUT_BYTES DP-1",
        "PREPARE_OUTPUT_BYTES DP 1 2",
        "SET_BYTES 18446744073709551616",
    };
    for (const auto& line : malformed) {
        NativeBinaryHeader header;
        std::string error;
        EXPECT_FALSE(parse_native_binary_header(line, &header, &error)) << line;
        EXPECT_FALSE(error.empty()) << line;
    }

    NativeBinaryHeader exact_limit;
    std::string error;
    ASSERT_TRUE(parse_native_binary_header(
        "SET_BYTES 134217728", &exact_limit, &error
    )) << error;
    EXPECT_EQ(exact_limit.payload_size, kNativeMaxSourceBytes);
    EXPECT_FALSE(parse_native_binary_header(
        "SET_BYTES 134217729", &exact_limit, &error
    ));

    NativeWallpaperProtocolDecoder decoder;
    std::vector<NativeWallpaperInput> inputs;
    EXPECT_FALSE(decoder.feed("SET_BYTES 134217729\n", &inputs, &error));
    EXPECT_TRUE(inputs.empty());
    EXPECT_FALSE(decoder.feed("SET_BYTES 0\n", &inputs, &error));
}

TEST(NativeWallpaperProtocolTest, TruncationAndLineBoundsCleanDecoderState) {
    NativeWallpaperProtocolDecoder truncated;
    std::vector<NativeWallpaperInput> inputs;
    std::string error;
    ASSERT_TRUE(truncated.feed("SET_BYTES 4\nabc", &inputs, &error)) << error;
    EXPECT_FALSE(truncated.finish(&error));
    EXPECT_FALSE(error.empty());

    NativeWallpaperProtocolDecoder oversized;
    std::string oversized_line(kNativeMaxCommandLineBytes + 1, 'x');
    EXPECT_FALSE(oversized.feed(oversized_line, &inputs, &error));
    EXPECT_TRUE(inputs.empty());
    EXPECT_FALSE(oversized.feed("SET legacy\n", &inputs, &error));
}

TEST(NativeWallpaperTransportTest, OwnedGlobalSetReplaysPayloadAfterHelperRestart) {
    RendererEnvironment environment;
    environment.exit_after_set();
    auto backend = std::make_shared<NativeWallpaperBackend>();
    std::string error;
    ASSERT_TRUE(backend->initialize(&error)) << error;
    ASSERT_TRUE(backend->set_wallpaper(
        WallpaperSource::owned_bytes("/missing/mutable-display-name.png", "owned-pixels"),
        &error
    )) << error;

    ASSERT_TRUE(wait_for_records(environment.log_path(), 2));
    const auto records = read_records(environment.log_path());
    ASSERT_GE(records.size(), 2U);
    EXPECT_EQ(records[0], "SET_BYTES 12|owned-pixels");
    EXPECT_EQ(records[1], "SET_BYTES 12|owned-pixels");
}

TEST(NativeWallpaperTransportTest, ExternalPathUsesLegacyCompatibilityCommand) {
    RendererEnvironment environment;
    auto backend = std::make_shared<NativeWallpaperBackend>();
    std::string error;
    ASSERT_TRUE(backend->initialize(&error)) << error;
    ASSERT_TRUE(backend->set_wallpaper(
        WallpaperSource(std::filesystem::path("/external/wallpaper.png")), &error
    )) << error;

    ASSERT_TRUE(wait_for_records(environment.log_path(), 1));
    const auto records = read_records(environment.log_path());
    ASSERT_EQ(records.size(), 1U);
    EXPECT_TRUE(std::string_view(records[0]).starts_with("SET "));
    EXPECT_FALSE(std::string_view(records[0]).starts_with("SET_BYTES "));
}

TEST(NativeWallpaperTransportTest, InvalidSourcesFailClosedWithoutCreatingPreparedState) {
    RendererEnvironment environment;
    auto backend = std::make_shared<NativeWallpaperBackend>();
    std::string error;
    EXPECT_FALSE(backend->set_wallpaper(WallpaperSource{}, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(backend->prepare_wallpaper(
        WallpaperSource::owned_bytes("/display/empty.png", {}), &error
    ));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(backend->prepare_wallpaper_for_output(
        WallpaperSource::owned_bytes("/display/valid.png", "pixels"),
        WallpaperOutputTarget{},
        &error
    ));
    EXPECT_FALSE(error.empty());
    backend->discard_prepared_wallpaper();
    EXPECT_TRUE(read_records(environment.log_path()).empty());
}

TEST(NativeWallpaperTransportTest, GlobalAndOutputTransactionsCommitOrDiscardPreparedState) {
    RendererEnvironment environment;
    auto backend = std::make_shared<NativeWallpaperBackend>();
    std::string error;
    ASSERT_TRUE(backend->initialize(&error)) << error;

    const auto owned = WallpaperSource::owned_bytes("/display/global.png", "global-pixels");
    ASSERT_TRUE(backend->prepare_wallpaper(owned, &error)) << error;
    ASSERT_TRUE(backend->commit_prepared_wallpaper(&error)) << error;
    ASSERT_TRUE(backend->prepare_wallpaper_for_output(
        WallpaperSource::owned_bytes("/display/output.png", "output-pixels"),
        WallpaperOutputTarget{-1, "DP-1"},
        &error
    )) << error;
    backend->discard_prepared_wallpaper();

    ASSERT_TRUE(wait_for_records(environment.log_path(), 4));
    const auto records = read_records(environment.log_path());
    ASSERT_GE(records.size(), 4U);
    EXPECT_EQ(records[0], "PREPARE_BYTES 13|global-pixels");
    EXPECT_EQ(records[1], "COMMIT");
    EXPECT_TRUE(std::string_view(records[2]).starts_with("PREPARE_OUTPUT_BYTES RFAtMQ== 13|output-pixels"));
    EXPECT_EQ(records[3], "DISCARD");
}

TEST(NativeWallpaperTransportTest, OutputReplayRetainsOwnedPayloadAndExternalOutputStaysLegacy) {
    RendererEnvironment environment;
    environment.exit_after_commit();
    auto backend = std::make_shared<NativeWallpaperBackend>();
    std::string error;
    ASSERT_TRUE(backend->initialize(&error)) << error;
    ASSERT_TRUE(backend->prepare_wallpaper_for_output(
        WallpaperSource::owned_bytes("/missing/output.png", "replay-output"),
        WallpaperOutputTarget{-1, "HDMI-A-1"},
        &error
    )) << error;
    ASSERT_TRUE(backend->commit_prepared_wallpaper(&error)) << error;

    ASSERT_TRUE(wait_for_records(environment.log_path(), 4));
    const auto records = read_records(environment.log_path());
    ASSERT_GE(records.size(), 4U);
    EXPECT_TRUE(std::string_view(records[0]).starts_with("PREPARE_OUTPUT_BYTES SERNSS1BLTE= 13|replay-output"));
    EXPECT_EQ(records[1], "COMMIT");
    EXPECT_TRUE(std::string_view(records[2]).starts_with("PREPARE_OUTPUT_BYTES SERNSS1BLTE= 13|replay-output"));
    EXPECT_EQ(records[3], "COMMIT");

    RendererEnvironment external_environment;
    auto external_backend = std::make_shared<NativeWallpaperBackend>();
    ASSERT_TRUE(external_backend->initialize(&error)) << error;
    ASSERT_TRUE(external_backend->prepare_wallpaper_for_output(
        WallpaperSource(std::filesystem::path("/external/output.png")),
        WallpaperOutputTarget{-1, "DP-1"},
        &error
    )) << error;
    ASSERT_TRUE(wait_for_records(external_environment.log_path(), 1));
    const auto external_records = read_records(external_environment.log_path());
    ASSERT_EQ(external_records.size(), 1U);
    EXPECT_TRUE(std::string_view(external_records[0]).starts_with("PREPARE_OUTPUT "));
}

} // namespace
