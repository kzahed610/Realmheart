#include "relictombs/RelictombsProtocol.hpp"

#include <gtest/gtest.h>

namespace realmheart::relictombs {
namespace {

TEST(RelictombsProtocolTests, CancelRoundTrips) {
    const auto parsed = parse_relictombs_result(
        serialize_relictombs_result({RelictombsResultKind::Cancel, {}})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, RelictombsResultKind::Cancel);
    EXPECT_TRUE(parsed->payload.empty());
}

TEST(RelictombsProtocolTests, ApplyPathWithWhitespaceTabsAndUnicodeRoundTrips) {
    const std::string path = "/tmp/Relictombs arch/壁紙\tfinal.webp";
    const auto parsed = parse_relictombs_result(
        serialize_relictombs_result({RelictombsResultKind::Apply, path})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, RelictombsResultKind::Apply);
    EXPECT_EQ(parsed->payload, path);
}

TEST(RelictombsProtocolTests, CommitPathRoundTrips) {
    const std::string path = "/tmp/Relictombs arch/selected final.png";
    const auto parsed = parse_relictombs_result(
        serialize_relictombs_result({RelictombsResultKind::Commit, path})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, RelictombsResultKind::Commit);
    EXPECT_EQ(parsed->payload, path);
}

TEST(RelictombsProtocolTests, CompleteRoundTrips) {
    const auto parsed = parse_relictombs_result(
        serialize_relictombs_result({RelictombsResultKind::Complete, {}})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, RelictombsResultKind::Complete);
    EXPECT_TRUE(parsed->payload.empty());
}

TEST(RelictombsProtocolTests, ErrorRoundTrips) {
    const std::string message = "renderer failed: weird\nmessage";
    const auto parsed = parse_relictombs_result(
        serialize_relictombs_result({RelictombsResultKind::Error, message})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, RelictombsResultKind::Error);
    EXPECT_EQ(parsed->payload, message);
}

TEST(RelictombsProtocolTests, OpenCommandPathRoundTrips) {
    const std::string path = "/home/user/Pictures/Wallpapers/Arthur & Regis.png";
    const auto parsed = parse_relictombs_command(
        serialize_relictombs_command({RelictombsCommandKind::Open, path})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, RelictombsCommandKind::Open);
    EXPECT_EQ(parsed->payload, path);
}

TEST(RelictombsProtocolTests, PrepareCommandPathRoundTrips) {
    const std::string path = "/home/user/Pictures/Wallpapers/世界 current.png";
    const auto parsed = parse_relictombs_command(
        serialize_relictombs_command({RelictombsCommandKind::Prepare, path})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, RelictombsCommandKind::Prepare);
    EXPECT_EQ(parsed->payload, path);
}

TEST(RelictombsProtocolTests, ControlCommandsRoundTrip) {
    const RelictombsCommandKind kinds[] = {
        RelictombsCommandKind::Close,
        RelictombsCommandKind::ApplyPrepared,
        RelictombsCommandKind::ApplyCommitted,
        RelictombsCommandKind::Refresh,
    };
    for (const auto kind : kinds) {
        const auto parsed = parse_relictombs_command(
            serialize_relictombs_command({kind, {}})
        );
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->kind, kind);
        EXPECT_TRUE(parsed->payload.empty());
    }
}

TEST(RelictombsProtocolTests, ApplyFailureDiagnosticRoundTrips) {
    const std::string diagnostic = "backend rejected /tmp/壁紙.png";
    const auto parsed = parse_relictombs_command(
        serialize_relictombs_command({RelictombsCommandKind::ApplyFailed, diagnostic})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, RelictombsCommandKind::ApplyFailed);
    EXPECT_EQ(parsed->payload, diagnostic);
}

TEST(RelictombsProtocolTests, RejectsUnknownRecord) {
    EXPECT_FALSE(parse_relictombs_result("WAT something").has_value());
    EXPECT_FALSE(parse_relictombs_command("WAT something").has_value());
}

} // namespace
} // namespace realmheart::relictombs
