#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace realmheart::events {

enum class Severity { Info, Success, Warning, Critical };
enum class Presentation { Ambient, Attention, Persistent };
enum class LifecycleState { Active, Resolved };
enum class ProgressMode { None, Indeterminate, Determinate };
enum class ActionKind { Registered, Uri, Copy };

struct Source {
    std::string id;
    std::string name;
    std::string icon;
};

struct Field {
    std::string label;
    std::string value;
};

struct Details {
    std::string format = "plain";
    std::string text;
};

struct Progress {
    ProgressMode mode = ProgressMode::None;
    double value = 0.0;
    std::string label;
};

struct EventAction {
    std::string id;
    std::string label;
    ActionKind kind = ActionKind::Registered;
    std::string uri;
    std::string value;
};

struct Lifecycle {
    LifecycleState state = LifecycleState::Active;
    bool persistent = false;
    bool acknowledged = false;
};

struct Event {
    std::string id;
    Source source;
    Severity severity = Severity::Info;
    Presentation presentation = Presentation::Ambient;
    std::string title;
    std::string summary;
    std::string timestamp;
    std::vector<Field> fields;
    std::optional<Details> details;
    std::optional<Progress> progress;
    std::vector<EventAction> actions;
    Lifecycle lifecycle;
    std::uint64_t revision = 0;
};

struct EventKey {
    std::string source_id;
    std::string event_id;

    friend bool operator==(const EventKey&, const EventKey&) = default;
};

struct EventKeyHash {
    std::size_t operator()(const EventKey& key) const noexcept;
};

std::string to_string(Severity value);
std::string to_string(Presentation value);
std::string to_string(LifecycleState value);
std::string to_string(ProgressMode value);
std::string to_string(ActionKind value);

std::optional<Severity> parse_severity(const std::string& value);
std::optional<Presentation> parse_presentation(const std::string& value);
std::optional<LifecycleState> parse_lifecycle_state(const std::string& value);
std::optional<ProgressMode> parse_progress_mode(const std::string& value);
std::optional<ActionKind> parse_action_kind(const std::string& value);

} // namespace realmheart::events
