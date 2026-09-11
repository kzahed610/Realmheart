#include "eventd/EventDaemonServer.hpp"
#include "events/EventClient.hpp"
#include "events/EventProtocol.hpp"
#include "events/EventTransport.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
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
    char root_template[] = "/tmp/realmheart-event-test-XXXXXX";
    char* root = ::mkdtemp(root_template);
    require(root != nullptr, "mkdtemp must succeed");
    const std::filesystem::path root_path(root);
    const auto runtime = root_path / "runtime";
    const auto state = root_path / "state";
    std::filesystem::create_directories(runtime);
    std::filesystem::create_directories(state);
    ::setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1);
    ::setenv("XDG_STATE_HOME", state.c_str(), 1);

    std::string error;
    {
        realmheart::eventd::EventDaemonServer server;
        require(server.start(error), "daemon must start");
        std::thread server_thread([&server] { server.run(); });

        std::mutex mutex;
        std::condition_variable changed;
        std::vector<std::string> message_types;
        realmheart::events::EventSubscriber subscriber;
        subscriber.start([&](const realmheart::events::Json& message) {
            std::lock_guard lock(mutex);
            message_types.push_back(message.value("type", "response"));
            changed.notify_all();
        });

        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, std::chrono::seconds(2), [&] {
                return !message_types.empty();
            }), "subscriber must receive initial snapshot");
            require(message_types.front() == "snapshot", "first subscriber message must be snapshot");
        }

        bool action_listener_connected = false;
        realmheart::events::Json received_invocation;
        realmheart::events::EventActionListener action_listener;
        action_listener.start(
            "integration",
            [&](const realmheart::events::Json& invocation) {
                std::lock_guard lock(mutex);
                received_invocation = invocation;
                changed.notify_all();
            },
            [&](bool connected) {
                std::lock_guard lock(mutex);
                action_listener_connected = connected;
                changed.notify_all();
            }
        );
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, std::chrono::seconds(2), [&] {
                return action_listener_connected;
            }), "producer action listener must register");
        }

        realmheart::events::Json create{
            {"protocol", 1},
            {"op", "create"},
            {"event", {
                {"id", "demo.build"},
                {"source", {{"id", "integration"}, {"name", "Integration"}}},
                {"title", "Building"},
                {"presentation", "attention"},
                {"actions", realmheart::events::Json::array({
                    {{"id", "retry"}, {"label", "Retry build"}, {"kind", "registered"}},
                    {{"id", "docs"}, {"label", "Open docs"}, {"kind", "uri"}, {"uri", "https://example.com"}}
                })}
            }}
        };
        auto response = realmheart::events::EventClient::request(create, error);
        require(response.value("ok", false), "create request must succeed");
        require(response["event"]["actions"].size() == 2U, "daemon response must preserve producer actions");
        require(response["event"]["actions"][0]["id"] == "retry", "daemon must preserve action order");

        realmheart::events::Json invoke{
            {"protocol", 1},
            {"op", "invoke_action"},
            {"source_id", "integration"},
            {"event_id", "demo.build"},
            {"action_id", "retry"}
        };
        response = realmheart::events::EventClient::request(invoke, error);
        require(response.value("ok", false) && response.value("status", "") == "routed",
                "registered action must route to connected producer");
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, std::chrono::seconds(2), [&] {
                return received_invocation.value("type", "") == "ACTION_INVOKED";
            }), "producer must receive ACTION_INVOKED");
            require(received_invocation.value("event_id", "") == "demo.build", "invocation must include event id");
            require(received_invocation.value("action_id", "") == "retry", "invocation must include action id");
        }

        realmheart::events::Json invoke_uri = invoke;
        invoke_uri["action_id"] = "docs";
        response = realmheart::events::EventClient::request(invoke_uri, error);
        require(!response.value("ok", true) && response["error"].value("code", "") == "action_ui_owned",
                "daemon must refuse invocation routing for UI-owned URI actions");

        realmheart::events::Json update{
            {"protocol", 1},
            {"op", "update"},
            {"source_id", "integration"},
            {"event_id", "demo.build"},
            {"patch", {
                {"progress", {{"mode", "determinate"}, {"value", 0.63}}},
                {"actions", realmheart::events::Json::array({
                    {{"id", "docs"}, {"label", "Docs first"}, {"kind", "uri"}, {"uri", "https://example.com/docs"}},
                    {{"id", "retry"}, {"label", "Retry second"}, {"kind", "registered"}},
                    {{"id", "copy"}, {"label", "Copy log"}, {"kind", "copy"}, {"value", "build log"}}
                })}
            }}
        };
        response = realmheart::events::EventClient::request(update, error);
        require(response.value("ok", false), "update request must succeed");
        require(response["event"]["actions"].size() == 3U && response["event"]["actions"][0]["id"] == "docs",
                "update must replace and reorder actions");

        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, std::chrono::seconds(2), [&] {
                return message_types.size() >= 3;
            }), "subscriber must receive create and update deltas");
            require(message_types[1] == "EVENT_CREATED", "create delta must be streamed");
            require(message_types[2] == "EVENT_UPDATED", "update delta must be streamed");
        }

        realmheart::events::Json resolve{
            {"protocol", 1},
            {"op", "resolve"},
            {"source_id", "integration"},
            {"event_id", "demo.build"},
            {"patch", {{"title", "Build complete"}}}
        };
        response = realmheart::events::EventClient::request(resolve, error);
        require(response.value("ok", false), "resolve request must succeed");

        realmheart::events::Json persistent{
            {"protocol", 1},
            {"op", "create"},
            {"event", {
                {"id", "restart.probe"},
                {"source", {{"id", "integration-persist"}, {"name", "Integration Persist"}}},
                {"title", "Survive daemon restart"},
                {"presentation", "persistent"},
                {"lifecycle", {{"persistent", true}}}
            }}
        };
        response = realmheart::events::EventClient::request(persistent, error);
        require(response.value("ok", false), "persistent event create must succeed");
        const auto persisted_revision = response["event"].value("revision", 0ULL);

        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, std::chrono::seconds(2), [&] {
                return message_types.size() >= 5;
            }), "subscriber must receive resolve and persistent create deltas");
            require(message_types[3] == "EVENT_RESOLVED", "resolve delta must be streamed");
            require(message_types[4] == "EVENT_CREATED", "persistent create delta must be streamed");
        }

        response = realmheart::events::EventClient::request({{"protocol", 1}, {"op", "status"}}, error);
        require(response.value("ok", false) && response.value("database", "") == "healthy",
                "status must report healthy persistence");
        require(response.value("subscribers", 0U) >= 1U, "status must expose connected subscribers");

        action_listener.stop();
        subscriber.stop();
        server.stop();
        if (server_thread.joinable()) server_thread.join();
        require(!std::filesystem::exists(realmheart::events::default_socket_path()),
                "daemon shutdown must remove runtime socket");

        // Preserve the expected floor for the restart checks below.
        require(persisted_revision > 0U, "persistent event must have a revision");
    }

    {
        realmheart::eventd::EventDaemonServer server;
        require(server.start(error), "daemon must restart with same state directory");
        std::thread server_thread([&server] { server.run(); });

        auto response = realmheart::events::EventClient::request({{"protocol", 1}, {"op", "list"}}, error);
        require(response.value("ok", false), "list after restart must succeed");
        require(response["events"].size() == 1U, "only persistent active event must replay after daemon restart");
        require(response["events"][0].value("id", "") == "restart.probe", "persistent event identity must survive restart");

        response = realmheart::events::EventClient::request({
            {"protocol", 1}, {"op", "inspect"}, {"source_id", "integration-persist"}, {"event_id", "restart.probe"}
        }, error);
        require(response.value("ok", false), "restored event must be inspectable");
        const auto restored_revision = response["event"].value("revision", 0ULL);

        response = realmheart::events::EventClient::request({{"protocol", 1}, {"op", "history"}, {"limit", 100}}, error);
        require(response.value("ok", false), "history query must succeed");
        require(response["events"].size() == 1U && response["events"][0].value("id", "") == "demo.build",
                "resolved event must remain in history after daemon restart");

        response = realmheart::events::EventClient::request({{"protocol", 1}, {"op", "sources"}}, error);
        require(response.value("ok", false) && response["sources"].size() >= 2U,
                "source registry must persist observed producers");

        response = realmheart::events::EventClient::request({
            {"protocol", 1}, {"op", "dismiss"}, {"source_id", "integration-persist"}, {"event_id", "restart.probe"}
        }, error);
        require(response.value("ok", false), "dismiss must succeed after restore");
        require(response["event"].value("revision", 0ULL) > restored_revision,
                "restored revision sequence must continue monotonically");

        response = realmheart::events::EventClient::request({{"protocol", 1}, {"op", "history"}, {"limit", 100}}, error);
        require(response.value("ok", false) && response["events"].size() == 2U,
                "dismissed event must be retained alongside resolved history");

        response = realmheart::events::EventClient::request({{"protocol", 1}, {"op", "clear_history"}}, error);
        require(response.value("ok", false), "clear history must succeed");
        response = realmheart::events::EventClient::request({{"protocol", 1}, {"op", "history"}}, error);
        require(response.value("ok", false) && response["events"].empty(), "history must be empty after clear");

        // User-trusted sources get 30 new creates per 10-second window by default.
        for (int index = 0; index < 30; ++index) {
            realmheart::events::Json flood{
                {"protocol", 1},
                {"op", "create"},
                {"event", {
                    {"id", "event-" + std::to_string(index)},
                    {"source", {{"id", "flood-source"}, {"name", "Flood Source"}}},
                    {"title", "Flood probe"}
                }}
            };
            response = realmheart::events::EventClient::request(flood, error);
            require(response.value("ok", false), "creates inside rate budget must succeed");
        }
        realmheart::events::Json overflow{
            {"protocol", 1},
            {"op", "create"},
            {"event", {
                {"id", "overflow"},
                {"source", {{"id", "flood-source"}, {"name", "Flood Source"}}},
                {"title", "Should throttle"}
            }}
        };
        response = realmheart::events::EventClient::request(overflow, error);
        require(!response.value("ok", true) && response["error"].value("code", "") == "rate_limited",
                "create flood must be rejected deterministically");

        response = realmheart::events::EventClient::request({{"protocol", 1}, {"op", "status"}}, error);
        require(response.value("rate_limited_sources", 0U) >= 1U, "status must expose rate-limited source count");

        server.stop();
        if (server_thread.joinable()) server_thread.join();
    }

    std::filesystem::remove_all(root_path);
    std::cout << "Event daemon integration tests passed\n";
    return 0;
}
