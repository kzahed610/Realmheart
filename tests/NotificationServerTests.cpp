#include "services/NotificationServer.hpp"

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        realmheart::services::NotificationHistory history(8);
        realmheart::services::NotificationServer server(history);

        const auto first = server.notify("mail", 0, "First", "Body one");
        const auto second = server.notify("chat", 0, "Second", "Body two");
        require(first == 1 && second == 2, "new notifications should receive monotonic non-zero ids");

        const auto replacement = server.notify("mail", first, "Updated", "Replacement body");
        require(replacement == first, "known replaces_id should retain its id");
        auto snapshot = history.snapshot();
        require(snapshot.entries.size() == 2, "replacement must not grow history");
        require(snapshot.entries.back().id == first, "replacement should become the newest entry");
        require(snapshot.entries.back().summary == "Updated", "replacement content should win");

        const auto unknown_replacement = server.notify("build", 99, "Build", "Done");
        require(unknown_replacement == 3, "unknown replaces_id should allocate a fresh id");

        require(server.close(second), "closing an active notification should succeed");
        require(!server.close(second), "closing an already inactive notification should fail");
        snapshot = history.snapshot();
        require(snapshot.entries.size() == 3, "closing a toast must preserve sidebar history");

        const auto replacement_after_close = server.notify(
            "chat", second, "New chat", "Closed ids are no longer replaceable"
        );
        require(replacement_after_close == 4, "a closed replaces_id should allocate a fresh id");
        require(history.snapshot().entries.size() == 4, "the fresh notification should append to history");

        std::string huge_app(realmheart::services::NotificationLimits::max_app_name_bytes + 100, 'a');
        std::string huge_summary(realmheart::services::NotificationLimits::max_summary_bytes + 100, 's');
        std::string huge_body(realmheart::services::NotificationLimits::max_body_bytes + 100, 'b');
        const auto bounded = server.notify(huge_app, 0, huge_summary, huge_body);
        snapshot = history.snapshot();
        const auto& bounded_entry = snapshot.entries.back();
        require(bounded_entry.id == bounded, "bounded notification should be stored");
        require(bounded_entry.app_name.size() <= realmheart::services::NotificationLimits::max_app_name_bytes,
                "app name payload must be bounded");
        require(bounded_entry.summary.size() <= realmheart::services::NotificationLimits::max_summary_bytes,
                "summary payload must be bounded");
        require(bounded_entry.body.size() <= realmheart::services::NotificationLimits::max_body_bytes,
                "body payload must be bounded");

        realmheart::services::NotificationHistory tiny_history(2);
        realmheart::services::NotificationServer tiny_server(tiny_history);
        std::uint32_t retired_id = 0;
        tiny_server.set_closed_handler([&](std::uint32_t id, std::uint32_t reason) {
            require(reason == 4, "capacity retirement should use the undefined close reason");
            retired_id = id;
        });
        const auto tiny_first = tiny_server.notify("one", 0, "one", "one");
        const auto tiny_second = tiny_server.notify("two", 0, "two", "two");
        static_cast<void>(tiny_second);
        tiny_server.notify("three", 0, "three", "three");
        require(retired_id == tiny_first, "active notification state must evict the oldest id");
        require(!tiny_server.close(tiny_first), "retired ids must no longer remain active");
    } catch (const std::exception& error) {
        std::cerr << "NotificationServerTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "NotificationServerTests passed\n";
    return 0;
}
