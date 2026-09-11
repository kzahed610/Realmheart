#include "events/EventTypes.hpp"

#include <functional>

namespace realmheart::events {

std::size_t EventKeyHash::operator()(const EventKey& key) const noexcept {
    const std::size_t first = std::hash<std::string>{}(key.source_id);
    const std::size_t second = std::hash<std::string>{}(key.event_id);
    return first ^ (second + 0x9e3779b97f4a7c15ULL + (first << 6U) + (first >> 2U));
}

std::string to_string(Severity value) {
    switch (value) {
    case Severity::Info: return "info";
    case Severity::Success: return "success";
    case Severity::Warning: return "warning";
    case Severity::Critical: return "critical";
    }
    return "info";
}

std::string to_string(Presentation value) {
    switch (value) {
    case Presentation::Ambient: return "ambient";
    case Presentation::Attention: return "attention";
    case Presentation::Persistent: return "persistent";
    }
    return "ambient";
}

std::string to_string(LifecycleState value) {
    switch (value) {
    case LifecycleState::Active: return "active";
    case LifecycleState::Resolved: return "resolved";
    }
    return "active";
}

std::string to_string(ProgressMode value) {
    switch (value) {
    case ProgressMode::None: return "none";
    case ProgressMode::Indeterminate: return "indeterminate";
    case ProgressMode::Determinate: return "determinate";
    }
    return "none";
}


std::string to_string(ActionKind value) {
    switch (value) {
    case ActionKind::Registered: return "registered";
    case ActionKind::Uri: return "uri";
    case ActionKind::Copy: return "copy";
    }
    return "registered";
}

std::optional<Severity> parse_severity(const std::string& value) {
    if (value == "info") return Severity::Info;
    if (value == "success") return Severity::Success;
    if (value == "warning") return Severity::Warning;
    if (value == "critical") return Severity::Critical;
    return std::nullopt;
}

std::optional<Presentation> parse_presentation(const std::string& value) {
    if (value == "ambient") return Presentation::Ambient;
    if (value == "attention") return Presentation::Attention;
    if (value == "persistent") return Presentation::Persistent;
    return std::nullopt;
}

std::optional<LifecycleState> parse_lifecycle_state(const std::string& value) {
    if (value == "active") return LifecycleState::Active;
    if (value == "resolved") return LifecycleState::Resolved;
    return std::nullopt;
}

std::optional<ProgressMode> parse_progress_mode(const std::string& value) {
    if (value == "none") return ProgressMode::None;
    if (value == "indeterminate") return ProgressMode::Indeterminate;
    if (value == "determinate") return ProgressMode::Determinate;
    return std::nullopt;
}

std::optional<ActionKind> parse_action_kind(const std::string& value) {
    if (value == "registered") return ActionKind::Registered;
    if (value == "uri") return ActionKind::Uri;
    if (value == "copy") return ActionKind::Copy;
    return std::nullopt;
}

} // namespace realmheart::events
