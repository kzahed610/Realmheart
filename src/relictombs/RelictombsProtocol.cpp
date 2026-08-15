#include "relictombs/RelictombsProtocol.hpp"

#include <glib.h>

namespace realmheart::relictombs {
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

std::string serialize_relictombs_result(const RelictombsResult& result) {
    switch (result.kind) {
    case RelictombsResultKind::Cancel:
        return "CANCEL\n";
    case RelictombsResultKind::Apply:
        return "APPLY " + encode(result.payload) + "\n";
    case RelictombsResultKind::Commit:
        return "COMMIT " + encode(result.payload) + "\n";
    case RelictombsResultKind::Complete:
        return "COMPLETE\n";
    case RelictombsResultKind::Error:
        return "ERROR " + encode(result.payload) + "\n";
    }
    return "ERROR \n";
}

std::optional<RelictombsResult> parse_relictombs_result(std::string_view line) {
    line = trim_line_end(line);
    if (line == "CANCEL") {
        return RelictombsResult{RelictombsResultKind::Cancel, {}};
    }
    if (line == "COMPLETE") {
        return RelictombsResult{RelictombsResultKind::Complete, {}};
    }

    constexpr std::string_view apply_prefix = "APPLY ";
    constexpr std::string_view commit_prefix = "COMMIT ";
    constexpr std::string_view error_prefix = "ERROR ";

    if (const auto payload = decode_prefixed_payload(line, apply_prefix)) {
        return RelictombsResult{RelictombsResultKind::Apply, *payload};
    }
    if (const auto payload = decode_prefixed_payload(line, commit_prefix)) {
        return RelictombsResult{RelictombsResultKind::Commit, *payload};
    }
    if (const auto payload = decode_prefixed_payload(line, error_prefix)) {
        return RelictombsResult{RelictombsResultKind::Error, *payload};
    }
    return std::nullopt;
}

std::string serialize_relictombs_command(const RelictombsCommand& command) {
    switch (command.kind) {
    case RelictombsCommandKind::Prepare:
        return "PREPARE " + encode(command.payload) + "\n";
    case RelictombsCommandKind::Open:
        return "OPEN " + encode(command.payload) + "\n";
    case RelictombsCommandKind::Close:
        return "CLOSE\n";
    case RelictombsCommandKind::ApplyPrepared:
        return "APPLY-PREPARED\n";
    case RelictombsCommandKind::ApplyCommitted:
        return "APPLY-COMMITTED\n";
    case RelictombsCommandKind::ApplyFailed:
        return "APPLY-FAILED " + encode(command.payload) + "\n";
    case RelictombsCommandKind::Refresh:
        return "REFRESH\n";
    }
    return "CLOSE\n";
}

std::optional<RelictombsCommand> parse_relictombs_command(std::string_view line) {
    line = trim_line_end(line);
    if (line == "CLOSE") {
        return RelictombsCommand{RelictombsCommandKind::Close, {}};
    }
    if (line == "APPLY-PREPARED") {
        return RelictombsCommand{RelictombsCommandKind::ApplyPrepared, {}};
    }
    if (line == "APPLY-COMMITTED") {
        return RelictombsCommand{RelictombsCommandKind::ApplyCommitted, {}};
    }
    if (line == "REFRESH") {
        return RelictombsCommand{RelictombsCommandKind::Refresh, {}};
    }

    constexpr std::string_view prepare_prefix = "PREPARE ";
    constexpr std::string_view open_prefix = "OPEN ";
    constexpr std::string_view failed_prefix = "APPLY-FAILED ";
    if (const auto payload = decode_prefixed_payload(line, prepare_prefix)) {
        return RelictombsCommand{RelictombsCommandKind::Prepare, *payload};
    }
    if (const auto payload = decode_prefixed_payload(line, open_prefix)) {
        return RelictombsCommand{RelictombsCommandKind::Open, *payload};
    }
    if (const auto payload = decode_prefixed_payload(line, failed_prefix)) {
        return RelictombsCommand{RelictombsCommandKind::ApplyFailed, *payload};
    }
    return std::nullopt;
}

} // namespace realmheart::relictombs
