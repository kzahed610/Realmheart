#include "events/EventProtocol.hpp"

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
    ValidationResult validation;
    Json input{
        {"id", "build.realmheart"},
        {"source", {{"id", "test-build"}, {"name", "Test Build"}}},
        {"title", "Building Realmheart"},
        {"summary", "Compile in progress"},
        {"severity", "info"},
        {"presentation", "attention"},
        {"progress", {{"mode", "determinate"}, {"value", 0.43}, {"label", "43%"}}},
        {"actions", Json::array({
            {{"id", "retry"}, {"label", "Retry"}, {"kind", "registered"}},
            {{"id", "docs"}, {"label", "Open docs"}, {"kind", "uri"}, {"uri", "https://example.com/docs"}},
            {{"id", "copy"}, {"label", "Copy details"}, {"kind", "copy"}, {"value", "diagnostic text"}}
        })}
    };
    const auto event = event_from_json(input, validation);
    require(event.has_value() && validation.ok, "valid event must parse");
    require(event->progress && event->progress->value == 0.43, "progress must round trip");
    require(event->actions.size() == 3U, "all producer actions must parse");
    require(event->actions[0].id == "retry" && event->actions[1].id == "docs" && event->actions[2].id == "copy",
            "producer action order must be preserved");
    require(event->actions[1].uri == "https://example.com/docs", "uri action target must parse");
    require(event->actions[2].value == "diagnostic text", "copy action value must parse");

    const auto encoded = event_to_json(*event);
    const auto reparsed = event_from_json(encoded, validation);
    require(reparsed.has_value() && reparsed->id == event->id, "serialized event must parse");
    require(reparsed->actions.size() == 3U && reparsed->actions[1].label == "Open docs",
            "actions must survive round-trip serialization");

    Json reordered = Json::array({
        {{"id", "copy"}, {"label", "Copy first"}, {"kind", "copy"}, {"value", "x"}},
        {{"id", "retry"}, {"label", "Retry second"}, {"kind", "registered"}}
    });
    Event mutable_event = *event;
    require(apply_event_patch(mutable_event, {{"actions", reordered}}).ok,
            "action list replacement must be accepted");
    require(mutable_event.actions.size() == 2U && mutable_event.actions[0].id == "copy" && mutable_event.actions[1].id == "retry",
            "action list replacement must preserve producer order");
    require(apply_event_patch(mutable_event, {{"actions", Json::array()}}).ok && mutable_event.actions.empty(),
            "producer must be able to remove all actions");

    Json invalid_title = input;
    invalid_title["title"] = std::string(kMaxTitleBytes + 1U, 'x');
    require(!event_from_json(invalid_title, validation).has_value(), "oversized title must be rejected");

    Json invalid_progress{{"progress", {{"mode", "determinate"}, {"value", 1.5}}}};
    mutable_event = *event;
    require(!apply_event_patch(mutable_event, invalid_progress).ok, "out-of-range progress must be rejected");

    Json duplicate_actions = input;
    duplicate_actions["actions"] = Json::array({
        {{"id", "same"}, {"label", "One"}, {"kind", "registered"}},
        {{"id", "same"}, {"label", "Two"}, {"kind", "registered"}}
    });
    require(!event_from_json(duplicate_actions, validation).has_value(), "duplicate action ids must be rejected");

    Json shell_action = input;
    shell_action["actions"] = Json::array({
        {{"id", "nope"}, {"label", "Nope"}, {"kind", "shell"}, {"value", "rm -rf /"}}
    });
    require(!event_from_json(shell_action, validation).has_value(), "shell action kind must be rejected");

    Json unsafe_uri = input;
    unsafe_uri["actions"] = Json::array({
        {{"id", "bad"}, {"label", "Bad"}, {"kind", "uri"}, {"uri", "javascript:alert(1)"}}
    });
    require(!event_from_json(unsafe_uri, validation).has_value(), "unsafe uri scheme must be rejected");

    Json too_many = input;
    too_many["actions"] = Json::array();
    for (std::size_t index = 0; index < kMaxActions + 1U; ++index) {
        too_many["actions"].push_back({
            {"id", "action-" + std::to_string(index)},
            {"label", "Action"},
            {"kind", "registered"}
        });
    }
    require(!event_from_json(too_many, validation).has_value(), "action count above protocol ceiling must be rejected");

    std::cout << "Event protocol tests passed\n";
    return 0;
}
