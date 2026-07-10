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

        require(server.close(second), "closing a known notification should succeed");
        require(!server.close(second), "closing an already removed notification should fail");
        snapshot = history.snapshot();
        require(snapshot.entries.size() == 2, "close should remove exactly one entry");
    } catch (const std::exception& error) {
        std::cerr << "NotificationServerTests failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "NotificationServerTests passed\n";
    return 0;
}
