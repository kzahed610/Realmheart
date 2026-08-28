#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "services/EmojiData.hpp"

namespace {

// Minimal glyph check: a data line starts with an emoji codepoint (outside the
// BMP or a variation selector) after the marker. We deliberately do a cheap
// line-based count rather than re-implement emoji grapheme parsing — the real
// tokenization is exercised by FuzzelEmojiScriptTests.
std::size_t count_emoji_entries(std::string_view data) {
    const std::size_t marker = data.find("### DATA ###");
    if (marker == std::string::npos) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = marker;
    while ((pos = data.find('\n', pos)) != std::string_view::npos) {
        ++pos;
        const std::size_t line_start = pos;
        const std::size_t line_end = data.find('\n', pos);
        if (line_end == std::string_view::npos) {
            break;
        }
        const std::string_view line = data.substr(line_start, line_end - line_start);
        if (!line.empty() && static_cast<unsigned char>(line[0]) > 0x7F) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST(EmojiDataFallback, IsPresentAndMarked) {
    const std::string data(realmheart::services::kEmojiDataFallback);
    ASSERT_FALSE(data.empty());
    EXPECT_NE(data.find("### DATA ###"), std::string::npos)
        << "built-in emoji index lost its data marker";
    EXPECT_LT(data.size(), 2u * 1024u * 1024u);
}

TEST(EmojiDataFallback, ContainsEnoughGlyphs) {
    const std::size_t count =
        count_emoji_entries(realmheart::services::kEmojiDataFallback);
    // The fallback carries the full ~1900 glyph index so a fresh account never
    // degrades to a ~150 entry subset. Guard against accidental truncation.
    EXPECT_GE(count, 1500u) << "emoji fallback index is suspiciously small";
}

