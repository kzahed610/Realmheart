#include "services/Notifications.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

realmheart::services::NotificationEntry notification(
    std::uint32_t id,
    std::string summary,
    bool unread = true
) {
    return {id, "realmheart-test", std::move(summary), "body", unread};
}

void test_history_is_bounded_and_evicts_oldest_entries() {
    realmheart::services::NotificationHistory history(2);
    history.upsert(notification(1, "one"));
    history.upsert(notification(2, "two"));
    history.upsert(notification(3, "three"));

    const auto snapshot = history.snapshot();
    require(snapshot.entries.size() == 2, "history must honor its configured capacity");
    require(snapshot.entries[0].id == 2 && snapshot.entries[1].id == 3,
            "capacity eviction must remove the oldest entry first");
    require(snapshot.unread_count == 2, "unread count must track retained entries only");
}

void test_replacement_updates_existing_entry_without_growing_history() {
    realmheart::services::NotificationHistory history(4);
    history.upsert(notification(7, "old"));
    history.upsert(notification(7, "replacement"));

    const auto snapshot = history.snapshot();
    require(snapshot.entries.size() == 1, "replacement id must update rather than duplicate");
    require(snapshot.entries.front().summary == "replacement", "replacement content must win");
    require(snapshot.unread_count == 1, "replacement notification must remain unread");
}

void test_read_dismiss_and_clear_keep_count_consistent() {
    realmheart::services::NotificationHistory history(4);
    history.upsert(notification(1, "one"));
    history.upsert(notification(2, "two", false));

    history.mark_all_read();
    require(history.snapshot().unread_count == 0, "mark_all_read must clear the unread count");
    require(history.dismiss(1), "dismiss must report an existing entry");
    require(!history.dismiss(99), "dismiss must report a missing entry");
    require(history.snapshot().entries.size() == 1, "dismiss must remove exactly one entry");

    history.clear();
    require(history.snapshot().entries.empty(), "clear must remove every entry");
}


void test_subscribers_receive_changes_and_can_unsubscribe() {
    realmheart::services::NotificationHistory history;
    int calls = 0;
    std::size_t last_unread = 0;
    auto subscription = history.subscribe([&](const auto& snapshot) {
        ++calls;
        last_unread = snapshot.unread_count;
    });

    history.upsert(notification(11, "live"));
    require(calls == 1 && last_unread == 1,
            "subscribers must receive live history updates");
    history.mark_all_read();
    require(calls == 2 && last_unread == 0,
            "read-state changes must be published");

    subscription.reset();
    history.clear();
    require(calls == 2, "reset subscriptions must stop receiving callbacks");
}

void test_capture_state_is_exposed_to_observers() {
    realmheart::services::NotificationHistory history;
    require(!history.snapshot().capture_active, "capture must start inactive during shell coexistence");
    history.set_capture_active(true);
    require(history.snapshot().capture_active, "capture state must be observable by the bar");
}

} // namespace

int main() {
    test_history_is_bounded_and_evicts_oldest_entries();
    test_replacement_updates_existing_entry_without_growing_history();
    test_read_dismiss_and_clear_keep_count_consistent();
    test_subscribers_receive_changes_and_can_unsubscribe();
    test_capture_state_is_exposed_to_observers();
    std::cout << "Notification history tests passed\n";
    return 0;
}
