#include "events/EventProtocol.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace realmheart::events {
namespace {

bool valid_text(const std::string& value, std::size_t limit, bool allow_empty = true) {
    return (allow_empty || !value.empty()) && value.size() <= limit;
}

ValidationResult parse_fields(const Json& json, std::vector<Field>& fields) {
    if (!json.is_array()) return ValidationResult::failure("invalid_fields", "fields must be an array");
    if (json.size() > kMaxFields) return ValidationResult::failure("too_many_fields", "fields exceeds protocol limit");
    std::vector<Field> parsed;
    parsed.reserve(json.size());
    for (const auto& item : json) {
        if (!item.is_object() || !item.contains("label") || !item.contains("value") ||
            !item["label"].is_string() || !item["value"].is_string()) {
            return ValidationResult::failure("invalid_field", "each field requires string label and value");
        }
        Field field{item["label"].get<std::string>(), item["value"].get<std::string>()};
        if (!valid_text(field.label, 256U, false) || !valid_text(field.value, 4096U)) {
            return ValidationResult::failure("field_too_large", "field label/value exceeds protocol limit");
        }
        parsed.push_back(std::move(field));
    }
    fields = std::move(parsed);
    return ValidationResult::success();
}

ValidationResult parse_actions(const Json& json, std::vector<EventAction>& actions) {
    if (!json.is_array()) return ValidationResult::failure("invalid_actions", "actions must be an array");
    if (json.size() > kMaxActions) return ValidationResult::failure("too_many_actions", "actions exceeds protocol limit");

    std::vector<EventAction> parsed;
    parsed.reserve(json.size());
    std::vector<std::string> ids;
    ids.reserve(json.size());
    for (const auto& item : json) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
            !item.contains("label") || !item["label"].is_string() ||
            !item.contains("kind") || !item["kind"].is_string()) {
            return ValidationResult::failure("invalid_action", "each action requires string id, label, and kind");
        }

        EventAction action;
        action.id = item["id"].get<std::string>();
        action.label = item["label"].get<std::string>();
        const auto kind = parse_action_kind(item["kind"].get<std::string>());
        if (!kind) return ValidationResult::failure("invalid_action_kind", "unknown action kind");
        action.kind = *kind;

        if (!valid_text(action.id, kMaxActionIdBytes, false)) {
            return ValidationResult::failure("invalid_action_id", "action id is empty or too large");
        }
        if (!valid_text(action.label, kMaxActionLabelBytes, false)) {
            return ValidationResult::failure("invalid_action_label", "action label is empty or too large");
        }
        if (std::find(ids.begin(), ids.end(), action.id) != ids.end()) {
            return ValidationResult::failure("duplicate_action_id", "action ids must be unique within an event");
        }
        ids.push_back(action.id);

        if (action.kind == ActionKind::Uri) {
            if (!item.contains("uri") || !item["uri"].is_string()) {
                return ValidationResult::failure("invalid_action_uri", "uri action requires string uri");
            }
            action.uri = item["uri"].get<std::string>();
            if (!valid_text(action.uri, kMaxActionUriBytes, false)) {
                return ValidationResult::failure("invalid_action_uri", "action uri is empty or too large");
            }
            const auto separator = action.uri.find(':');
            if (separator == std::string::npos || separator == 0) {
                return ValidationResult::failure("invalid_action_uri", "action uri requires an explicit scheme");
            }
            const std::string scheme = action.uri.substr(0, separator);
            if (scheme != "https" && scheme != "http" && scheme != "file" && scheme != "realmheart") {
                return ValidationResult::failure("disallowed_action_uri", "action uri scheme is not allowed");
            }
        } else if (action.kind == ActionKind::Copy) {
            if (!item.contains("value") || !item["value"].is_string()) {
                return ValidationResult::failure("invalid_action_value", "copy action requires string value");
            }
            action.value = item["value"].get<std::string>();
            if (!valid_text(action.value, kMaxActionValueBytes)) {
                return ValidationResult::failure("action_value_too_large", "copy action value exceeds protocol limit");
            }
        }
        parsed.push_back(std::move(action));
    }
    actions = std::move(parsed);
    return ValidationResult::success();
}

ValidationResult parse_details(const Json& json, std::optional<Details>& details) {
    if (json.is_null()) {
        details.reset();
        return ValidationResult::success();
    }
    if (!json.is_object()) return ValidationResult::failure("invalid_details", "details must be an object or null");
    Details parsed;
    parsed.format = json.value("format", "plain");
    if (parsed.format != "plain") {
        return ValidationResult::failure("unsupported_details_format", "v1 only supports plain details");
    }
    if (!json.contains("text") || !json["text"].is_string()) {
        return ValidationResult::failure("invalid_details", "details.text must be a string");
    }
    parsed.text = json["text"].get<std::string>();
    if (!valid_text(parsed.text, kMaxDetailsBytes)) {
        return ValidationResult::failure("details_too_large", "details exceeds protocol limit");
    }
    details = std::move(parsed);
    return ValidationResult::success();
}

ValidationResult parse_progress(const Json& json, std::optional<Progress>& progress) {
    if (json.is_null()) {
        progress.reset();
        return ValidationResult::success();
    }
    if (!json.is_object()) return ValidationResult::failure("invalid_progress", "progress must be an object or null");
    if (!json.contains("mode") || !json["mode"].is_string()) {
        return ValidationResult::failure("invalid_progress", "progress.mode must be a string");
    }
    const auto mode = parse_progress_mode(json["mode"].get<std::string>());
    if (!mode) return ValidationResult::failure("invalid_progress_mode", "unknown progress mode");
    Progress parsed;
    parsed.mode = *mode;
    parsed.label = json.value("label", "");
    if (!valid_text(parsed.label, 256U)) {
        return ValidationResult::failure("progress_label_too_large", "progress label exceeds protocol limit");
    }
    if (parsed.mode == ProgressMode::Determinate) {
        if (!json.contains("value") || !json["value"].is_number()) {
            return ValidationResult::failure("invalid_progress", "determinate progress requires numeric value");
        }
        parsed.value = json["value"].get<double>();
        if (!std::isfinite(parsed.value) || parsed.value < 0.0 || parsed.value > 1.0) {
            return ValidationResult::failure("invalid_progress_value", "progress value must be within 0.0..1.0");
        }
    }
    progress = std::move(parsed);
    return ValidationResult::success();
}

ValidationResult parse_lifecycle(const Json& json, Lifecycle& lifecycle) {
    if (!json.is_object()) return ValidationResult::failure("invalid_lifecycle", "lifecycle must be an object");
    Lifecycle parsed = lifecycle;
    if (json.contains("state")) {
        if (!json["state"].is_string()) return ValidationResult::failure("invalid_lifecycle", "lifecycle.state must be a string");
        const auto state = parse_lifecycle_state(json["state"].get<std::string>());
        if (!state) return ValidationResult::failure("invalid_lifecycle_state", "unknown lifecycle state");
        parsed.state = *state;
    }
    if (json.contains("persistent")) {
        if (!json["persistent"].is_boolean()) return ValidationResult::failure("invalid_lifecycle", "lifecycle.persistent must be boolean");
        parsed.persistent = json["persistent"].get<bool>();
    }
    if (json.contains("acknowledged")) {
        if (!json["acknowledged"].is_boolean()) return ValidationResult::failure("invalid_lifecycle", "lifecycle.acknowledged must be boolean");
        parsed.acknowledged = json["acknowledged"].get<bool>();
    }
    lifecycle = parsed;
    return ValidationResult::success();
}

} // namespace

Json event_to_json(const Event& event) {
    Json fields = Json::array();
    for (const auto& field : event.fields) fields.push_back({{"label", field.label}, {"value", field.value}});

    Json actions = Json::array();
    for (const auto& action : event.actions) {
        Json encoded{{"id", action.id}, {"label", action.label}, {"kind", to_string(action.kind)}};
        if (action.kind == ActionKind::Uri) encoded["uri"] = action.uri;
        if (action.kind == ActionKind::Copy) encoded["value"] = action.value;
        actions.push_back(std::move(encoded));
    }

    Json details = nullptr;
    if (event.details) details = {{"format", event.details->format}, {"text", event.details->text}};

    Json progress = nullptr;
    if (event.progress) {
        progress = {{"mode", to_string(event.progress->mode)}, {"label", event.progress->label}};
        if (event.progress->mode == ProgressMode::Determinate) progress["value"] = event.progress->value;
    }

    return {
        {"id", event.id},
        {"source", {{"id", event.source.id}, {"name", event.source.name}, {"icon", event.source.icon}}},
        {"severity", to_string(event.severity)},
        {"presentation", to_string(event.presentation)},
        {"title", event.title},
        {"summary", event.summary},
        {"timestamp", event.timestamp},
        {"fields", std::move(fields)},
        {"details", std::move(details)},
        {"progress", std::move(progress)},
        {"actions", std::move(actions)},
        {"lifecycle", {
            {"state", to_string(event.lifecycle.state)},
            {"persistent", event.lifecycle.persistent},
            {"acknowledged", event.lifecycle.acknowledged}
        }},
        {"revision", event.revision}
    };
}

std::optional<Event> event_from_json(const Json& json, ValidationResult& validation) {
    if (!json.is_object()) {
        validation = ValidationResult::failure("invalid_event", "event must be an object");
        return std::nullopt;
    }
    if (!json.contains("id") || !json["id"].is_string() ||
        !json.contains("source") || !json["source"].is_object() ||
        !json["source"].contains("id") || !json["source"]["id"].is_string() ||
        !json.contains("title") || !json["title"].is_string()) {
        validation = ValidationResult::failure("missing_required_field", "event requires id, source.id, and title");
        return std::nullopt;
    }

    Event event;
    event.id = json["id"].get<std::string>();
    event.source.id = json["source"]["id"].get<std::string>();
    event.source.name = json["source"].value("name", event.source.id);
    event.source.icon = json["source"].value("icon", "");
    event.title = json["title"].get<std::string>();
    event.summary = json.value("summary", "");
    event.timestamp = json.value("timestamp", "");
    if (json.contains("revision") && json["revision"].is_number_unsigned()) event.revision = json["revision"].get<std::uint64_t>();

    if (json.contains("severity")) {
        if (!json["severity"].is_string()) {
            validation = ValidationResult::failure("invalid_severity", "severity must be a string");
            return std::nullopt;
        }
        const auto severity = parse_severity(json["severity"].get<std::string>());
        if (!severity) {
            validation = ValidationResult::failure("invalid_severity", "unknown severity");
            return std::nullopt;
        }
        event.severity = *severity;
    }
    if (json.contains("presentation")) {
        if (!json["presentation"].is_string()) {
            validation = ValidationResult::failure("invalid_presentation", "presentation must be a string");
            return std::nullopt;
        }
        const auto presentation = parse_presentation(json["presentation"].get<std::string>());
        if (!presentation) {
            validation = ValidationResult::failure("invalid_presentation", "unknown presentation class");
            return std::nullopt;
        }
        event.presentation = *presentation;
    }

    if (json.contains("fields")) {
        validation = parse_fields(json["fields"], event.fields);
        if (!validation.ok) return std::nullopt;
    }
    if (json.contains("details")) {
        validation = parse_details(json["details"], event.details);
        if (!validation.ok) return std::nullopt;
    }
    if (json.contains("progress")) {
        validation = parse_progress(json["progress"], event.progress);
        if (!validation.ok) return std::nullopt;
    }
    if (json.contains("actions")) {
        validation = parse_actions(json["actions"], event.actions);
        if (!validation.ok) return std::nullopt;
    }
    if (json.contains("lifecycle")) {
        validation = parse_lifecycle(json["lifecycle"], event.lifecycle);
        if (!validation.ok) return std::nullopt;
    }

    validation = validate_create_event(event);
    if (!validation.ok) return std::nullopt;
    return event;
}

ValidationResult validate_create_event(const Event& event) {
    if (!valid_text(event.id, kMaxEventIdBytes, false)) return ValidationResult::failure("invalid_event_id", "event id is empty or too large");
    if (!valid_text(event.source.id, kMaxSourceIdBytes, false)) return ValidationResult::failure("invalid_source_id", "source id is empty or too large");
    if (!valid_text(event.title, kMaxTitleBytes, false)) return ValidationResult::failure("invalid_title", "title is empty or too large");
    if (!valid_text(event.summary, kMaxSummaryBytes)) return ValidationResult::failure("summary_too_large", "summary exceeds protocol limit");
    if (event.fields.size() > kMaxFields) return ValidationResult::failure("too_many_fields", "fields exceeds protocol limit");
    if (event.actions.size() > kMaxActions) return ValidationResult::failure("too_many_actions", "actions exceeds protocol limit");
    if (event.details && !valid_text(event.details->text, kMaxDetailsBytes)) return ValidationResult::failure("details_too_large", "details exceeds protocol limit");
    return ValidationResult::success();
}

ValidationResult validate_request_envelope(const Json& request) {
    if (!request.is_object()) return ValidationResult::failure("invalid_request", "request must be a JSON object");
    if (!request.contains("protocol") || !request["protocol"].is_number_integer()) return ValidationResult::failure("missing_protocol", "request requires integer protocol");
    if (request["protocol"].get<int>() != kProtocolVersion) return ValidationResult::failure("unsupported_protocol", "unsupported protocol major version");
    if (!request.contains("op") || !request["op"].is_string()) return ValidationResult::failure("missing_operation", "request requires string op");
    return ValidationResult::success();
}

ValidationResult apply_event_patch(Event& event, const Json& patch) {
    if (!patch.is_object()) return ValidationResult::failure("invalid_patch", "patch must be an object");
    if (patch.contains("id") || patch.contains("source")) return ValidationResult::failure("immutable_identity", "event id/source cannot be changed by update");

    if (patch.contains("title")) {
        if (!patch["title"].is_string()) return ValidationResult::failure("invalid_title", "title must be a string");
        event.title = patch["title"].get<std::string>();
    }
    if (patch.contains("summary")) {
        if (!patch["summary"].is_string()) return ValidationResult::failure("invalid_summary", "summary must be a string");
        event.summary = patch["summary"].get<std::string>();
    }
    if (patch.contains("timestamp")) {
        if (!patch["timestamp"].is_string()) return ValidationResult::failure("invalid_timestamp", "timestamp must be a string");
        event.timestamp = patch["timestamp"].get<std::string>();
    }
    if (patch.contains("severity")) {
        if (!patch["severity"].is_string()) return ValidationResult::failure("invalid_severity", "severity must be a string");
        const auto parsed = parse_severity(patch["severity"].get<std::string>());
        if (!parsed) return ValidationResult::failure("invalid_severity", "unknown severity");
        event.severity = *parsed;
    }
    if (patch.contains("presentation")) {
        if (!patch["presentation"].is_string()) return ValidationResult::failure("invalid_presentation", "presentation must be a string");
        const auto parsed = parse_presentation(patch["presentation"].get<std::string>());
        if (!parsed) return ValidationResult::failure("invalid_presentation", "unknown presentation class");
        event.presentation = *parsed;
    }
    ValidationResult result;
    if (patch.contains("fields")) {
        result = parse_fields(patch["fields"], event.fields);
        if (!result.ok) return result;
    }
    if (patch.contains("details")) {
        result = parse_details(patch["details"], event.details);
        if (!result.ok) return result;
    }
    if (patch.contains("progress")) {
        result = parse_progress(patch["progress"], event.progress);
        if (!result.ok) return result;
    }
    if (patch.contains("actions")) {
        result = parse_actions(patch["actions"], event.actions);
        if (!result.ok) return result;
    }
    if (patch.contains("lifecycle")) {
        result = parse_lifecycle(patch["lifecycle"], event.lifecycle);
        if (!result.ok) return result;
    }
    return validate_create_event(event);
}

Json success_response(const std::string& op, const Json& payload) {
    Json response{{"protocol", kProtocolVersion}, {"ok", true}, {"op", op}};
    for (auto it = payload.begin(); it != payload.end(); ++it) response[it.key()] = it.value();
    return response;
}

Json error_response(const std::string& code, const std::string& message) {
    return {{"protocol", kProtocolVersion}, {"ok", false}, {"error", {{"code", code}, {"message", message}}}};
}

std::string now_iso8601_utc() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

} // namespace realmheart::events
