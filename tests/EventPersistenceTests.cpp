#include "events/EventPersistence.hpp"
#include "events/EventProtocol.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    using namespace realmheart::events;
    char state_template[] = "/tmp/realmheart-event-persistence-XXXXXX";
    char* state = ::mkdtemp(state_template);
    require(state != nullptr, "mkdtemp must succeed");
    ::setenv("XDG_STATE_HOME", state, 1);

    std::string error;
    EventPersistence database;
    require(database.open(error), "database must open");
    require(std::filesystem::exists(database.path()), "database file must exist");

    Event active;
    active.id = "persist.active";
    active.source = {"persist-test", "Persist Test", ""};
    active.title = "Still running";
    active.lifecycle.persistent = true;
    active.revision = 9;
    active.timestamp = now_iso8601_utc();
    require(database.save_active(active, error), "active event must persist");

    Event resolved = active;
    resolved.id = "persist.resolved";
    resolved.title = "Complete";
    resolved.lifecycle.state = LifecycleState::Resolved;
    resolved.revision = 10;
    require(database.save_resolved(resolved, error), "resolved event must persist");

    Event dismissed = active;
    dismissed.id = "persist.dismissed";
    dismissed.revision = 11;
    require(database.save_dismissed(dismissed, error), "dismissed event must persist in history");

    SourceRecord source;
    source.id = "persist-test";
    source.display_name = "Persist Test";
    source.uid = static_cast<std::uint32_t>(::getuid());
    source.last_pid = static_cast<std::int64_t>(::getpid());
    source.executable = "/tmp/persist-test";
    source.trust_class = "user";
    source.first_seen = now_iso8601_utc();
    source.last_seen = source.first_seen;
    require(database.upsert_source(source, error), "source must persist");

    database.close();
    require(database.open(error), "database must reopen");
    const auto restored = database.load_active(error);
    require(error.empty(), "active restore must not error");
    require(restored.size() == 1U && restored.front().id == "persist.active", "only persistent active event must restore");
    require(database.max_revision(error) == 11U, "maximum revision must survive restart");

    const auto history = database.history(100U, error);
    require(error.empty(), "history query must not error");
    require(history.size() == 2U, "resolved and dismissed events must be historical");
    require(history[0].id == "persist.dismissed" && history[1].id == "persist.resolved", "history must be revision ordered");

    const auto sources = database.sources(error);
    require(sources.size() == 1U && sources.front().id == "persist-test", "source registry must survive restart");

    require(database.clear_history(error), "history clear must succeed");
    require(database.history(100U, error).empty(), "history clear must preserve no historical rows");
    require(database.load_active(error).size() == 1U, "history clear must not delete active persistent events");

    require(database.erase({"persist-test", "persist.active"}, error), "active event delete must succeed");
    require(database.load_active(error).empty(), "deleted event must not restore");

    database.close();
    std::filesystem::remove_all(state);
    std::cout << "Event persistence tests passed\n";
    return 0;
}
