#pragma once

#include "events/EventTypes.hpp"
#include "nlohmann_json/json.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace realmheart::events {

inline constexpr int kProtocolVersion = 1;
inline constexpr std::size_t kMaxPayloadBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxTitleBytes = 256U;
inline constexpr std::size_t kMaxSummaryBytes = 4U * 1024U;
inline constexpr std::size_t kMaxDetailsBytes = 256U * 1024U;
inline constexpr std::size_t kMaxFields = 64U;
inline constexpr std::size_t kMaxActions = 8U;
inline constexpr std::size_t kMaxActionIdBytes = 128U;
inline constexpr std::size_t kMaxActionLabelBytes = 256U;
inline constexpr std::size_t kMaxActionUriBytes = 8U * 1024U;
inline constexpr std::size_t kMaxActionValueBytes = 64U * 1024U;
inline constexpr std::size_t kMaxSourceIdBytes = 128U;
inline constexpr std::size_t kMaxEventIdBytes = 256U;

using Json = nlohmann::json;

struct ValidationResult {
    bool ok = true;
    std::string code;
    std::string message;

    static ValidationResult success() { return {}; }
    static ValidationResult failure(std::string code, std::string message) {
        return {false, std::move(code), std::move(message)};
    }
};

Json event_to_json(const Event& event);
std::optional<Event> event_from_json(const Json& json, ValidationResult& validation);
ValidationResult validate_create_event(const Event& event);
ValidationResult validate_request_envelope(const Json& request);
ValidationResult apply_event_patch(Event& event, const Json& patch);

Json success_response(const std::string& op, const Json& payload = Json::object());
Json error_response(const std::string& code, const std::string& message);
std::string now_iso8601_utc();

} // namespace realmheart::events
