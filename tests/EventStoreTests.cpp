#include "events/EventStore.hpp"

#include <cstdlib>
#include <iostream>

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
    EventStore store;
    Event event;
    event.id = "demo.build";
    event.source = {"demo", "Demo", ""};
    event.title = "Building";
    event.presentation = Presentation::Attention;

    const auto created = store.create(event);
    require(created.event.has_value(), "create must succeed");
    const auto first_revision = created.event->revision;
    require(store.snapshot().size() == 1, "create must produce one active event");

    const auto replaced = store.create(event);
    require(replaced.event.has_value(), "replacement create must succeed");
    require(store.snapshot().size() == 1, "same identity must replace rather than duplicate");
    require(replaced.event->revision > first_revision, "replacement must advance revision");

    const auto updated = store.update({"demo", "demo.build"}, {{"progress", {{"mode", "determinate"}, {"value", 0.5}}}});
    require(updated.event && updated.event->progress && updated.event->progress->value == 0.5, "update must patch existing event");

    const auto acknowledged = store.acknowledge({"demo", "demo.build"});
    require(acknowledged.event && acknowledged.event->lifecycle.acknowledged, "acknowledge must not resolve event");
    require(store.snapshot().size() == 1, "acknowledged event remains active");

    const auto resolved = store.resolve({"demo", "demo.build"}, {{"title", "Complete"}});
    require(resolved.event && resolved.event->lifecycle.state == LifecycleState::Resolved, "resolve must produce resolved final state");
    require(store.snapshot().empty(), "resolved event must leave active set");

    Event restored;
    restored.id = "restored";
    restored.source = {"restore-source", "Restore", ""};
    restored.title = "Restored";
    restored.lifecycle.persistent = true;
    restored.revision = 50;
    store.restore({restored}, 70);
    require(store.snapshot().size() == 1U, "restore must repopulate persistent active state");
    require(store.sequence() == 70U, "restore must preserve persisted sequence floor");
    const auto post_restore = store.update({"restore-source", "restored"}, {{"summary", "after restart"}});
    require(post_restore.event && post_restore.event->revision == 71U, "post-restart revision must remain monotonic");
    require(store.active_count_for_source("restore-source") == 1U, "source active count must be available to rate limiter");

    std::cout << "Event store tests passed\n";
    return 0;
}
