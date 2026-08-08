#include "worldscar/WorldscarProtocol.hpp"

#include <gtest/gtest.h>

namespace realmheart::worldscar {
namespace {

TEST(WorldscarProtocolTests, CancelRoundTrips) {
    const auto parsed = parse_worldscar_result(
        serialize_worldscar_result({WorldscarResultKind::Cancel, {}})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, WorldscarResultKind::Cancel);
    EXPECT_TRUE(parsed->payload.empty());
}

TEST(WorldscarProtocolTests, ApplyPathWithWhitespaceTabsAndUnicodeRoundTrips) {
    const std::string path = "/tmp/World scar/壁紙\tfinal.webp";
    const auto parsed = parse_worldscar_result(
        serialize_worldscar_result({WorldscarResultKind::Apply, path})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, WorldscarResultKind::Apply);
    EXPECT_EQ(parsed->payload, path);
}

TEST(WorldscarProtocolTests, CommitPathRoundTrips) {
    const std::string path = "/tmp/World scar/selected final.png";
    const auto parsed = parse_worldscar_result(
        serialize_worldscar_result({WorldscarResultKind::Commit, path})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, WorldscarResultKind::Commit);
    EXPECT_EQ(parsed->payload, path);
}

TEST(WorldscarProtocolTests, CompleteRoundTrips) {
    const auto parsed = parse_worldscar_result(
        serialize_worldscar_result({WorldscarResultKind::Complete, {}})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, WorldscarResultKind::Complete);
    EXPECT_TRUE(parsed->payload.empty());
}

TEST(WorldscarProtocolTests, ErrorRoundTrips) {
    const std::string message = "shader failed: weird\nmessage";
    const auto parsed = parse_worldscar_result(
        serialize_worldscar_result({WorldscarResultKind::Error, message})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, WorldscarResultKind::Error);
    EXPECT_EQ(parsed->payload, message);
}

TEST(WorldscarProtocolTests, OpenCommandPathRoundTrips) {
    const std::string path = "/home/user/Pictures/Wallpapers/Arthur & Regis.png";
    const auto parsed = parse_worldscar_command(
        serialize_worldscar_command({WorldscarCommandKind::Open, path})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, WorldscarCommandKind::Open);
    EXPECT_EQ(parsed->payload, path);
}

TEST(WorldscarProtocolTests, PrepareCommandPathRoundTrips) {
    const std::string path = "/home/user/Pictures/Wallpapers/世界 current.png";
    const auto parsed = parse_worldscar_command(
        serialize_worldscar_command({WorldscarCommandKind::Prepare, path})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, WorldscarCommandKind::Prepare);
    EXPECT_EQ(parsed->payload, path);
}

TEST(WorldscarProtocolTests, ControlCommandsRoundTrip) {
    const WorldscarCommandKind kinds[] = {
        WorldscarCommandKind::Close,
        WorldscarCommandKind::ApplyPrepared,
        WorldscarCommandKind::ApplyCommitted,
        WorldscarCommandKind::Refresh,
    };
    for (const auto kind : kinds) {
        const auto parsed = parse_worldscar_command(
            serialize_worldscar_command({kind, {}})
        );
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed->kind, kind);
        EXPECT_TRUE(parsed->payload.empty());
    }
}

TEST(WorldscarProtocolTests, ApplyFailureDiagnosticRoundTrips) {
    const std::string diagnostic = "backend rejected /tmp/壁紙.png";
    const auto parsed = parse_worldscar_command(
        serialize_worldscar_command({WorldscarCommandKind::ApplyFailed, diagnostic})
    );
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, WorldscarCommandKind::ApplyFailed);
    EXPECT_EQ(parsed->payload, diagnostic);
}

TEST(WorldscarProtocolTests, RejectsUnknownRecord) {
    EXPECT_FALSE(parse_worldscar_result("WAT something").has_value());
    EXPECT_FALSE(parse_worldscar_command("WAT something").has_value());
}

} // namespace
} // namespace realmheart::worldscar
