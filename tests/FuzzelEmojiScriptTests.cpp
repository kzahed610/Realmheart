// tests/FuzzelEmojiScriptTests.cpp
//
// End-to-end parse check against the emoji data file shipped with the
// installer. Catches two regressions at once:
//   * the file format breaking on updates
//   * the file accidentally being dropped from the repo

#include "services/LauncherService.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path resolve_shipped_script() {
    // Try a few plausible locations so the test works whether it was
    // launched from the build dir (CTest) or the source root (direct
    // invocation, IDE debugger, etc.).
    const std::filesystem::path candidates[] = {
        "../config/hypr/hyprland/scripts/fuzzel-emoji.sh",
        "config/hypr/hyprland/scripts/fuzzel-emoji.sh",
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return std::filesystem::absolute(candidate);
        }
    }
    return std::filesystem::absolute(candidates[0]);
}

std::string slurp(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()
    );
}

TEST(FuzzelEmojiScript, ShippedFileExistsAndIsExecutable) {
    const auto path = resolve_shipped_script();
    ASSERT_TRUE(std::filesystem::exists(path)) << path;

    // installer copies with 755; the in-tree file must match so the bit
    // doesn't get accidentally stripped on commit.
    const auto perms = std::filesystem::status(path).permissions();
    const bool owner_exec = (perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
    EXPECT_TRUE(owner_exec);
}

TEST(FuzzelEmojiScript, ContainsStandaloneDataMarker) {
    const auto path = resolve_shipped_script();
    ASSERT_TRUE(std::filesystem::exists(path)) << path;

    const std::string contents = slurp(path);
    constexpr std::string_view marker = "### DATA ###";

    // Marker must appear on its own line so the launcher's
    // data_start_after_standalone_marker() recognises it.
    bool found_standalone = false;
    std::size_t line_start = 0;
    while (line_start <= contents.size()) {
        const auto line_end = contents.find('\n', line_start);
        const auto line = std::string_view(contents).substr(
            line_start,
            line_end == std::string_view::npos ? contents.size() - line_start : line_end - line_start
        );
        const auto trimmed = line;
        if (trimmed == marker) {
            found_standalone = true;
            break;
        }
        if (line_end == std::string_view::npos) break;
        line_start = line_end + 1;
    }
    EXPECT_TRUE(found_standalone);
}

TEST(FuzzelEmojiScript, ParserFindsEmojiFromShippedFile) {
    const auto path = resolve_shipped_script();
    ASSERT_TRUE(std::filesystem::exists(path)) << path;

    const std::string contents = slurp(path);

    // Empty filter should return the first page of glyphs (up to the
    // launcher's internal cap). Verify we got a meaningful number back.
    auto all = realmheart::services::launcher_emoji_results(contents, "", 200);
    EXPECT_GT(all.size(), 50u);

    // Filter by a keyword that we know the file contains.
    auto cat = realmheart::services::launcher_emoji_results(contents, "cat", 50);
    EXPECT_GT(cat.size(), 1u);
    bool found_cat_face = false;
    for (const auto& result : cat) {
        if (result.title.find("cat") != std::string::npos ||
            result.description.find("cat") != std::string::npos) {
            found_cat_face = true;
            break;
        }
    }
    EXPECT_TRUE(found_cat_face);
}

TEST(FuzzelEmojiScript, DoesNotExceedTwoMegabytes) {
    // The launcher rejects files larger than 2 MB at load time. Catch a
    // runaway append well before that surfaces as a runtime failure.
    const auto path = resolve_shipped_script();
    ASSERT_TRUE(std::filesystem::exists(path)) << path;
    const auto size = std::filesystem::file_size(path);
    EXPECT_LT(size, 2u * 1024u * 1024u);
}

} // namespace// force rebuild
