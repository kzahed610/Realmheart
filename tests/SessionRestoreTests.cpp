#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "services/SessionRestore.hpp"

using namespace realmheart::services;
namespace fs = std::filesystem;

namespace {

fs::path temporary_root() {
    return fs::temp_directory_path() /
           ("realmheart-session-restore-" + std::to_string(::getpid()));
}

SessionRestoreEntry make_entry(
    std::string app_class,
    int workspace,
    std::string directory,
    std::vector<std::string> argv
) {
    SessionRestoreEntry entry;
    entry.app_class = std::move(app_class);
    entry.title = entry.app_class + " window";
    entry.workspace_id = workspace;
    entry.working_directory = std::move(directory);
    entry.argv = std::move(argv);
    return entry;
}

} // namespace

TEST(SessionRestoreTest, EntryIsRestorableOnlyWithUsableArgvAndWorkspace) {
    EXPECT_TRUE(make_entry("kitty", 3, "/tmp", {"kitty"}).restorable());
    EXPECT_FALSE(make_entry("", 3, "/tmp", {"kitty"}).restorable());
    EXPECT_FALSE(make_entry("kitty", 0, "/tmp", {"kitty"}).restorable());
    EXPECT_FALSE(make_entry("kitty", 3, "/tmp", {}).restorable());
    // A bare command name is resolved by the shell, so it stays valid.
    EXPECT_TRUE(make_entry("zen", 1, "/tmp", {"zen-bin"}).restorable());
    // An absolute path that does not exist cannot be launched.
    EXPECT_FALSE(make_entry("ghost", 1, "/tmp", {"/nonexistent/ghost"}).restorable());
}

TEST(SessionRestoreTest, RestorableEntriesDropsIgnoredAndDuplicateWindows) {
    std::vector<SessionRestoreEntry> entries;
    entries.push_back(make_entry("kitty", 3, "/home/zahed", {"kitty"}));
    entries.push_back(make_entry("kitty", 3, "/home/zahed", {"kitty"}));
    entries.push_back(make_entry("org.kde.dolphin", 2, "/home/zahed", {"dolphin"}));
    entries.push_back(make_entry("realmheart", 4, "/home/zahed", {"realmheart", "--shell"}));
    entries.push_back(make_entry("xdg-desktop-portal-hyprland", 1, "/tmp", {"xdg-desktop-portal-hyprland"}));
    entries.push_back(make_entry("broken", 0, "/tmp", {"broken"}));

    const auto kept = restorable_entries(std::move(entries));
    ASSERT_EQ(kept.size(), 2u);
    EXPECT_EQ(kept[0].app_class, "kitty");
    EXPECT_EQ(kept[1].app_class, "org.kde.dolphin");
}

TEST(SessionRestoreTest, LaunchCommandPreservesArgumentsAndDirectory) {
    const auto entry = make_entry(
        "org.kde.dolphin",
        2,
        "/home/zahed/Projects/Godot-Prototype",
        {"dolphin", "/home/zahed/Projects/Godot-Prototype"}
    );
    const std::string command = build_launch_command(entry);
    EXPECT_EQ(
        command,
        "cd '/home/zahed/Projects/Godot-Prototype' && exec 'dolphin' "
        "'/home/zahed/Projects/Godot-Prototype'"
    );
}

TEST(SessionRestoreTest, LaunchCommandQuotesHostileArguments) {
    const auto entry = make_entry(
        "kitty",
        3,
        "/home/zahed/it's here",
        {"kitty", "a b", "it's", ""}
    );
    const std::string command = build_launch_command(entry);
    EXPECT_NE(command.find("'a b'"), std::string::npos);
    EXPECT_NE(command.find("'it'\\''s'"), std::string::npos);
    // An empty argument survives as an explicit empty literal.
    EXPECT_NE(command.find("''"), std::string::npos);
}

TEST(SessionRestoreTest, LaunchCommandOmitsMissingDirectory) {
    const auto entry = make_entry(
        "kitty",
        3,
        "/home/zahed/definitely-not-a-real-directory",
        {"kitty"}
    );
    EXPECT_EQ(build_launch_command(entry), " 'kitty'");
}

TEST(SessionRestoreTest, SnapshotRoundTripsThroughJson) {
    SessionRestoreSnapshot snapshot;
    snapshot.available = true;
    snapshot.boot_id = "boot-abc";
    snapshot.captured_at_epoch = 1700000000;
    snapshot.entries.push_back(make_entry("kitty", 3, "/tmp", {"kitty", "--hold"}));

    const auto document = snapshot.to_json();
    const auto restored = SessionRestoreSnapshot::from_json(document);

    ASSERT_TRUE(restored.available);
    EXPECT_EQ(restored.boot_id, "boot-abc");
    EXPECT_EQ(restored.captured_at_epoch, 1700000000);
    ASSERT_EQ(restored.entries.size(), 1u);
    EXPECT_EQ(restored.entries.front().app_class, "kitty");
    EXPECT_EQ(restored.entries.front().workspace_id, 3);
    ASSERT_EQ(restored.entries.front().argv.size(), 2u);
    EXPECT_EQ(restored.entries.front().argv.back(), "--hold");
}

TEST(SessionRestoreTest, FromJsonRejectsUnknownSchema) {
    const auto document = nlohmann::json{
        {"schema", 999},
        {"boot_id", "boot-abc"},
        {"entries", nlohmann::json::array()}
    };
    const auto restored = SessionRestoreSnapshot::from_json(document);
    EXPECT_FALSE(restored.available);
    EXPECT_FALSE(restored.error.empty());
}

TEST(SessionRestoreTest, PendingRestoreRequiresDifferentBoot) {
    SessionRestoreSnapshot snapshot;
    snapshot.available = true;
    snapshot.boot_id = "boot-old";
    snapshot.entries.push_back(make_entry("kitty", 3, "/tmp", {"kitty"}));

    EXPECT_TRUE(is_pending_restore(snapshot, "boot-new"));
    EXPECT_FALSE(is_pending_restore(snapshot, "boot-old"));

    const auto empty_boot = [] {
        SessionRestoreSnapshot value;
        value.available = true;
        value.boot_id = "boot-old";
        return value;
    }();
    EXPECT_FALSE(is_pending_restore(empty_boot, "boot-new"));

    const auto no_boot = [] {
        SessionRestoreSnapshot value;
        value.available = true;
        value.entries.push_back(make_entry("kitty", 3, "/tmp", {"kitty"}));
        return value;
    }();
    EXPECT_FALSE(is_pending_restore(no_boot, "boot-new"));
    EXPECT_FALSE(is_pending_restore(no_boot, ""));
}

class SessionRestoreStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = temporary_root() / "store";
        fs::remove_all(root_);
        fs::create_directories(root_);
    }

    void TearDown() override {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    [[nodiscard]] fs::path state_path() const { return root_ / "restore.json"; }

    fs::path root_;
};

TEST_F(SessionRestoreStoreTest, MissingFileLoadsAsNothing) {
    SessionRestoreStore store(state_path());
    std::string diagnostic = "stale";
    const auto loaded = store.load(&diagnostic);
    EXPECT_FALSE(loaded.has_value());
    EXPECT_TRUE(diagnostic.empty());
}

TEST_F(SessionRestoreStoreTest, SaveThenLoadPreservesSnapshot) {
    SessionRestoreStore store(state_path());

    SessionRestoreSnapshot snapshot;
    snapshot.available = true;
    snapshot.boot_id = "boot-old";
    snapshot.captured_at_epoch = 42;
    snapshot.entries.push_back(make_entry("zen", 1, "/tmp", {"zen"}));

    std::string diagnostic;
    ASSERT_TRUE(store.save(snapshot, &diagnostic)) << diagnostic;
    EXPECT_TRUE(fs::exists(state_path()));

    const auto loaded = store.load(&diagnostic);
    ASSERT_TRUE(loaded.has_value()) << diagnostic;
    ASSERT_EQ(loaded->entries.size(), 1u);
    EXPECT_EQ(loaded->boot_id, "boot-old");
    EXPECT_EQ(loaded->entries.front().app_class, "zen");
}

TEST_F(SessionRestoreStoreTest, SaveIsAtomicOnRepeat) {
    SessionRestoreStore store(state_path());
    SessionRestoreSnapshot snapshot;
    snapshot.available = true;
    snapshot.boot_id = "boot-one";
    snapshot.entries.push_back(make_entry("kitty", 2, "/tmp", {"kitty"}));

    ASSERT_TRUE(store.save(snapshot));
    snapshot.boot_id = "boot-two";
    ASSERT_TRUE(store.save(snapshot));

    // No partial file is left beside the destination.
    EXPECT_FALSE(fs::exists(fs::path(state_path().string() + ".partial")));
    const auto loaded = store.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->boot_id, "boot-two");
}

TEST_F(SessionRestoreStoreTest, CorruptFileIsRejectedThenClearable) {
    std::ofstream(state_path(), std::ios::binary | std::ios::trunc) << "{not json";

    SessionRestoreStore store(state_path());
    std::string diagnostic;
    EXPECT_FALSE(store.load(&diagnostic).has_value());
    EXPECT_FALSE(diagnostic.empty());

    EXPECT_TRUE(store.clear());
    EXPECT_FALSE(fs::exists(state_path()));
    // Clearing a missing snapshot is not an error.
    EXPECT_TRUE(store.clear());
}

TEST_F(SessionRestoreStoreTest, DefaultPathFollowsStateHomeOverride) {
    const fs::path override_path = fs::temp_directory_path() / "realmheart-restore-override.json";
    ::setenv("REALMHEART_SESSION_RESTORE_STATE", override_path.c_str(), 1);
    EXPECT_EQ(SessionRestoreStore::default_path(), override_path);
    ::unsetenv("REALMHEART_SESSION_RESTORE_STATE");
}