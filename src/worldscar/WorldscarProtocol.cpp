#include "worldscar/WorldscarProtocol.hpp"

#include <glib.h>

namespace realmheart::worldscar {
namespace {

std::string encode(std::string_view value) {
    gchar* encoded = g_base64_encode(
        reinterpret_cast<const guchar*>(value.data()),
        value.size()
    );
    if (encoded == nullptr) return {};
    std::string result = encoded;
    g_free(encoded);
    return result;
}

std::optional<std::string> decode(std::string_view value) {
    if (value.empty()) return std::string{};

    gsize decoded_size = 0;
    guchar* decoded = g_base64_decode(std::string(value).c_str(), &decoded_size);
    if (decoded == nullptr) return std::nullopt;

    std::string result(
        reinterpret_cast<const char*>(decoded),
        static_cast<std::size_t>(decoded_size)
    );
    g_free(decoded);
    return result;
}

std::string_view trim_line_end(std::string_view value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

std::optional<std::string> decode_prefixed_payload(
    std::string_view line,
    std::string_view prefix
) {
    if (!line.starts_with(prefix)) return std::nullopt;
    return decode(line.substr(prefix.size()));
}

} // namespace

std::string serialize_worldscar_result(const WorldscarResult& result) {
    switch (result.kind) {
    case WorldscarResultKind::Cancel:
        return "CANCEL\n";
    case WorldscarResultKind::Apply:
        return "APPLY " + encode(result.payload) + "\n";
    case WorldscarResultKind::Commit:
        return "COMMIT " + encode(result.payload) + "\n";
    case WorldscarResultKind::Complete:
        return "COMPLETE\n";
    case WorldscarResultKind::Error:
        return "ERROR " + encode(result.payload) + "\n";
    }
    return "ERROR \n";
}

std::optional<WorldscarResult> parse_worldscar_result(std::string_view line) {
    line = trim_line_end(line);
    if (line == "CANCEL") {
        return WorldscarResult{WorldscarResultKind::Cancel, {}};
    }
    if (line == "COMPLETE") {
        return WorldscarResult{WorldscarResultKind::Complete, {}};
    }

    constexpr std::string_view apply_prefix = "APPLY ";
    constexpr std::string_view commit_prefix = "COMMIT ";
    constexpr std::string_view error_prefix = "ERROR ";

    if (const auto payload = decode_prefixed_payload(line, apply_prefix)) {
        return WorldscarResult{WorldscarResultKind::Apply, *payload};
    }
    if (const auto payload = decode_prefixed_payload(line, commit_prefix)) {
        return WorldscarResult{WorldscarResultKind::Commit, *payload};
    }
    if (const auto payload = decode_prefixed_payload(line, error_prefix)) {
        return WorldscarResult{WorldscarResultKind::Error, *payload};
    }
    return std::nullopt;
}

std::string serialize_worldscar_command(const WorldscarCommand& command) {
    switch (command.kind) {
    case WorldscarCommandKind::Prepare:
        return "PREPARE " + encode(command.payload) + "\n";
    case WorldscarCommandKind::Open:
        return "OPEN " + encode(command.payload) + "\n";
    case WorldscarCommandKind::Close:
        return "CLOSE\n";
    case WorldscarCommandKind::ApplyPrepared:
        return "APPLY-PREPARED\n";
    case WorldscarCommandKind::ApplyCommitted:
        return "APPLY-COMMITTED\n";
    case WorldscarCommandKind::ApplyFailed:
        return "APPLY-FAILED " + encode(command.payload) + "\n";
    case WorldscarCommandKind::Refresh:
        return "REFRESH\n";
    }
    return "CLOSE\n";
}

std::optional<WorldscarCommand> parse_worldscar_command(std::string_view line) {
    line = trim_line_end(line);
    if (line == "CLOSE") {
        return WorldscarCommand{WorldscarCommandKind::Close, {}};
    }
    if (line == "APPLY-PREPARED") {
        return WorldscarCommand{WorldscarCommandKind::ApplyPrepared, {}};
    }
    if (line == "APPLY-COMMITTED") {
        return WorldscarCommand{WorldscarCommandKind::ApplyCommitted, {}};
    }
    if (line == "REFRESH") {
        return WorldscarCommand{WorldscarCommandKind::Refresh, {}};
    }

    constexpr std::string_view prepare_prefix = "PREPARE ";
    constexpr std::string_view open_prefix = "OPEN ";
    constexpr std::string_view failed_prefix = "APPLY-FAILED ";
    if (const auto payload = decode_prefixed_payload(line, prepare_prefix)) {
        return WorldscarCommand{WorldscarCommandKind::Prepare, *payload};
    }
    if (const auto payload = decode_prefixed_payload(line, open_prefix)) {
        return WorldscarCommand{WorldscarCommandKind::Open, *payload};
    }
    if (const auto payload = decode_prefixed_payload(line, failed_prefix)) {
        return WorldscarCommand{WorldscarCommandKind::ApplyFailed, *payload};
    }
    return std::nullopt;
}

} // namespace realmheart::worldscar
