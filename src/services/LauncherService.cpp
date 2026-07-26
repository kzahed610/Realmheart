#include "LauncherService.hpp"

#include "core/Command.hpp"
#include "services/HyprlandApplicationMonitor.hpp"
#include "services/HyprlandSession.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace realmheart::services {

namespace fs = std::filesystem;

namespace {

fs::path xdg_home_path(const char* variable, std::string_view fallback_suffix) {
    if (const char* configured = std::getenv(variable);
        configured != nullptr && *configured != '\0') {
        return fs::path(configured);
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return fs::path(home) / fallback_suffix;
    }
    return fs::temp_directory_path() / "realmheart";
}

fs::path actions_directory() {
    return xdg_home_path("XDG_CONFIG_HOME", ".config") / "realmheart/actions";
}

fs::path launcher_pins_path() {
    return xdg_home_path("XDG_CONFIG_HOME", ".config") /
        "realmheart/launcher-pins.txt";
}

fs::path launcher_history_path() {
    return xdg_home_path("XDG_STATE_HOME", ".local/state") /
        "realmheart/launcher-history.tsv";
}

char ascii_lower(char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

std::string lowercase_copy(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), ascii_lower);
    return lowered;
}

std::string trim_copy(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\n\r");
    return std::string(value.substr(first, last - first + 1));
}

std::size_t data_start_after_standalone_marker(
    std::string_view contents,
    std::string_view marker
) {
    std::size_t line_start = 0;
    while (line_start <= contents.size()) {
        const std::size_t line_end = contents.find('\n', line_start);
        std::string_view line = contents.substr(
            line_start,
            line_end == std::string_view::npos
                ? contents.size() - line_start
                : line_end - line_start
        );
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        const std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            const std::size_t last = line.find_last_not_of(" \t");
            if (line.substr(first, last - first + 1) == marker) {
                return line_end == std::string_view::npos
                    ? contents.size()
                    : line_end + 1;
            }
        }

        if (line_end == std::string_view::npos) break;
        line_start = line_end + 1;
    }
    return std::string_view::npos;
}

std::string normalized_copy(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    bool pending_space = false;

    for (const char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0) {
            if (pending_space && !normalized.empty()) normalized.push_back(' ');
            normalized.push_back(ascii_lower(character));
            pending_space = false;
        } else {
            pending_space = true;
        }
    }
    return normalized;
}

bool starts_at_word_boundary(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return false;
    std::size_t position = haystack.find(needle);
    while (position != std::string_view::npos) {
        if (position == 0 || haystack[position - 1] == ' ') return true;
        position = haystack.find(needle, position + 1);
    }
    return false;
}

std::string acronym_for(std::string_view value) {
    std::string acronym;
    bool at_word_start = true;
    for (const char character : value) {
        if (character == ' ') {
            at_word_start = true;
            continue;
        }
        if (at_word_start) acronym.push_back(character);
        at_word_start = false;
    }
    return acronym;
}

int fuzzy_subsequence_score(std::string_view value, std::string_view query) {
    if (query.empty() || query.size() > value.size()) return 0;

    std::size_t query_index = 0;
    int gaps = 0;
    int consecutive = 0;
    int best_consecutive = 0;
    std::size_t previous_match = std::numeric_limits<std::size_t>::max();

    for (std::size_t index = 0; index < value.size() && query_index < query.size(); ++index) {
        if (value[index] != query[query_index]) continue;

        if (previous_match != std::numeric_limits<std::size_t>::max()) {
            const int distance = static_cast<int>(index - previous_match - 1);
            gaps += distance;
            consecutive = distance == 0 ? consecutive + 1 : 1;
        } else {
            consecutive = 1;
        }
        best_consecutive = std::max(best_consecutive, consecutive);
        previous_match = index;
        ++query_index;
    }

    if (query_index != query.size()) return 0;
    return std::max(1, 1300 + best_consecutive * 70 - gaps * 18);
}

int score_field(std::string_view value, std::string_view query, int tier) {
    const std::string normalized_value = normalized_copy(value);
    if (normalized_value.empty()) return 0;

    if (normalized_value == query) return tier + 1000;
    if (normalized_value.starts_with(query)) return tier + 800;
    if (starts_at_word_boundary(normalized_value, query)) return tier + 620;
    if (normalized_value.find(query) != std::string::npos) return tier + 430;

    const std::string acronym = acronym_for(normalized_value);
    if (!acronym.empty()) {
        if (acronym == query) return tier + 390;
        if (acronym.starts_with(query)) return tier + 340;
    }

    const int fuzzy = fuzzy_subsequence_score(normalized_value, query);
    return fuzzy > 0 ? tier + fuzzy / 4 : 0;
}

bool running_inside_systemd_unit() {
    const char* invocation_id = std::getenv("INVOCATION_ID");
    return invocation_id != nullptr && *invocation_id != '\0';
}

std::string first_command_token(std::string_view command) {
    const auto first = command.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto end = command.find_first_of(" \t", first);
    return std::string(command.substr(
        first,
        end == std::string_view::npos ? command.size() - first : end - first
    ));
}

std::string compact_identity(std::string_view value) {
    std::string compact;
    for (const char character : normalized_copy(value)) {
        if (character != ' ') compact.push_back(character);
    }
    return compact;
}

std::string desktop_identity(std::string_view value) {
    std::string identity(value);
    constexpr std::string_view suffix = ".desktop";
    if (identity.size() >= suffix.size() &&
        std::string_view(identity).substr(identity.size() - suffix.size()) == suffix) {
        identity.resize(identity.size() - suffix.size());
    }
    return identity;
}

std::string executable_identity(std::string_view value) {
    const std::string token = first_command_token(value);
    if (token.empty()) return {};
    return fs::path(token).filename().string();
}

int hyprland_identity_score(std::string_view observed, std::string_view candidate) {
    const std::string observed_normalized = normalized_copy(observed);
    const std::string candidate_normalized = normalized_copy(candidate);
    if (observed_normalized.empty() || candidate_normalized.empty()) return 0;

    if (observed_normalized == candidate_normalized) return 1000;

    const std::string observed_compact = compact_identity(observed_normalized);
    const std::string candidate_compact = compact_identity(candidate_normalized);
    if (observed_compact == candidate_compact) return 950;

    // Hyprland classes often use reverse-DNS desktop IDs while the executable
    // is just the final component (org.kde.dolphin -> dolphin).
    if (observed_compact.size() >= 4 && candidate_compact.size() >= 4) {
        if (observed_compact.ends_with(candidate_compact) ||
            candidate_compact.ends_with(observed_compact)) {
            return 760;
        }
    }

    if (starts_at_word_boundary(candidate_normalized, observed_normalized) ||
        starts_at_word_boundary(observed_normalized, candidate_normalized)) {
        return 620;
    }
    return 0;
}

std::vector<std::string> split_tabs(std::string_view line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t tab = line.find('\t', start);
        fields.emplace_back(line.substr(
            start,
            tab == std::string_view::npos ? line.size() - start : tab - start
        ));
        if (tab == std::string_view::npos) break;
        start = tab + 1;
    }
    return fields;
}

struct CalculationEvaluation {
    std::string result;
    std::string display_expression;
};

class LauncherExpressionParser {
public:
    explicit LauncherExpressionParser(std::string_view input) : input_(input) {}

    std::optional<double> parse() {
        if (input_.empty() || input_.size() > 256) return std::nullopt;

        const auto value = parse_expression();
        skip_spaces();
        if (!value.has_value() || position_ != input_.size() || operator_count_ == 0 ||
            !std::isfinite(*value)) {
            return std::nullopt;
        }
        return value;
    }

private:
    std::optional<double> parse_expression() {
        auto left = parse_term();
        if (!left.has_value()) return std::nullopt;

        while (true) {
            skip_spaces();
            if (consume('+')) {
                auto right = parse_term();
                if (!right.has_value()) return std::nullopt;
                ++operator_count_;
                left = checked(*left + *right);
            } else if (consume('-')) {
                auto right = parse_term();
                if (!right.has_value()) return std::nullopt;
                ++operator_count_;
                left = checked(*left - *right);
            } else {
                return left;
            }
            if (!left.has_value()) return std::nullopt;
        }
    }

    std::optional<double> parse_term() {
        auto left = parse_unary();
        if (!left.has_value()) return std::nullopt;

        while (true) {
            skip_spaces();
            if (consume('*')) {
                auto right = parse_unary();
                if (!right.has_value()) return std::nullopt;
                ++operator_count_;
                left = checked(*left * *right);
            } else if (consume('/')) {
                auto right = parse_unary();
                if (!right.has_value() || *right == 0.0) return std::nullopt;
                ++operator_count_;
                left = checked(*left / *right);
            } else if (consume('%')) {
                auto right = parse_unary();
                if (!right.has_value() || *right == 0.0) return std::nullopt;
                ++operator_count_;
                left = checked(std::fmod(*left, *right));
            } else {
                return left;
            }
            if (!left.has_value()) return std::nullopt;
        }
    }

    std::optional<double> parse_unary() {
        skip_spaces();
        if (consume('+')) {
            ++operator_count_;
            return parse_unary();
        }
        if (consume('-')) {
            ++operator_count_;
            const auto value = parse_unary();
            return value.has_value() ? checked(-*value) : std::nullopt;
        }
        return parse_power();
    }

    std::optional<double> parse_power() {
        auto base = parse_primary();
        if (!base.has_value()) return std::nullopt;

        skip_spaces();
        if (!consume('^')) return base;

        ++operator_count_;
        const auto exponent = parse_unary();
        if (!exponent.has_value()) return std::nullopt;
        return checked(std::pow(*base, *exponent));
    }

    std::optional<double> parse_primary() {
        skip_spaces();
        if (consume('(')) {
            auto value = parse_expression();
            skip_spaces();
            if (!value.has_value() || !consume(')')) return std::nullopt;
            return value;
        }
        return parse_number();
    }

    std::optional<double> parse_number() {
        skip_spaces();
        if (position_ >= input_.size()) return std::nullopt;

        double value = 0.0;
        const char* const first = input_.data() + position_;
        const char* const last = input_.data() + input_.size();
        const auto [parsed_end, error] = std::from_chars(
            first,
            last,
            value,
            std::chars_format::general
        );
        if (error != std::errc{} || parsed_end == first || !std::isfinite(value)) {
            return std::nullopt;
        }
        position_ = static_cast<std::size_t>(parsed_end - input_.data());
        return value;
    }

    static std::optional<double> checked(double value) {
        return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    }

    void skip_spaces() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
            ++position_;
        }
    }

    bool consume(char character) {
        if (position_ >= input_.size() || input_[position_] != character) return false;
        ++position_;
        return true;
    }

    std::string_view input_;
    std::size_t position_ = 0;
    std::size_t operator_count_ = 0;
};

std::string format_calculation_result(double value) {
    if (value == 0.0) value = 0.0; // Normalize negative zero.

    std::array<char, 96> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(),
        buffer.data() + buffer.size(),
        value,
        std::chars_format::general,
        15
    );
    if (error != std::errc{}) return {};
    return std::string(buffer.data(), end);
}

std::string calculation_display_expression(std::string_view expression) {
    std::string display;
    display.reserve(expression.size() + 8);
    for (const char character : expression) {
        if (character == '*') {
            display += "×";
        } else if (character == '/') {
            display += "÷";
        } else {
            display.push_back(character);
        }
    }
    return display;
}

std::optional<CalculationEvaluation> evaluate_calculation(std::string_view expression) {
    LauncherExpressionParser parser(expression);
    const auto value = parser.parse();
    if (!value.has_value()) return std::nullopt;

    std::string result = format_calculation_result(*value);
    if (result.empty()) return std::nullopt;
    return CalculationEvaluation{
        .result = std::move(result),
        .display_expression = calculation_display_expression(expression),
    };
}

std::string clipboard_image_mime(std::string_view preview) {
    const std::string lowered = lowercase_copy(preview);
    constexpr std::array<std::string_view, 8> supported{
        "image/png",
        "image/jpeg",
        "image/jpg",
        "image/webp",
        "image/bmp",
        "image/gif",
        "image/tiff",
        "image/x-icon",
    };
    for (const auto mime : supported) {
        if (lowered.find(mime) == std::string::npos) continue;
        return mime == "image/jpg" ? "image/jpeg" : std::string(mime);
    }

    // Cliphist normally prints a compact binary marker with an image format token.
    if (lowered.find("binary") == std::string::npos) return {};
    constexpr std::array<std::pair<std::string_view, std::string_view>, 8> extensions{{
        {"png", "image/png"},
        {"jpeg", "image/jpeg"},
        {"jpg", "image/jpeg"},
        {"webp", "image/webp"},
        {"bmp", "image/bmp"},
        {"gif", "image/gif"},
        {"tiff", "image/tiff"},
        {"ico", "image/x-icon"},
    }};
    for (const auto& [extension, mime] : extensions) {
        if (lowered.find(extension) != std::string::npos) return std::string(mime);
    }
    return {};
}

bool clipboard_binary_preview(std::string_view preview) {
    const std::string lowered = lowercase_copy(preview);
    return lowered.find("[[ binary data ") != std::string::npos;
}

std::string clipboard_format_label(std::string_view mime) {
    if (mime == "image/jpeg" || mime == "image/jpg") return "JPEG";
    if (mime == "image/png") return "PNG";
    if (mime == "image/webp") return "WEBP";
    if (mime == "image/bmp") return "BMP";
    if (mime == "image/gif") return "GIF";
    if (mime == "image/tiff") return "TIFF";
    if (mime == "image/x-icon") return "ICO";
    return "IMAGE";
}

bool clipboard_filter_matches(const LauncherResult& result, std::string_view filter) {
    const std::string normalized_filter = normalized_copy(filter);
    if (normalized_filter.empty()) return true;

    if (normalized_copy(result.title).find(normalized_filter) != std::string::npos ||
        normalized_copy(result.subtitle).find(normalized_filter) != std::string::npos ||
        normalized_copy(result.description).find(normalized_filter) != std::string::npos ||
        normalized_copy(result.clipboard_mime).find(normalized_filter) != std::string::npos) {
        return true;
    }
    return std::ranges::any_of(result.search_terms, [&normalized_filter](const auto& term) {
        return normalized_copy(term).find(normalized_filter) != std::string::npos;
    });
}

std::string recommendation_identity(const LauncherResult& result) {
    return normalized_copy(result.title) + "\n" + normalized_copy(result.executable);
}

std::int64_t current_epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

bool ignored_session_identity(std::string_view identity) {
    const std::string normalized = compact_identity(identity);
    if (normalized.empty()) return true;

    constexpr std::array<std::string_view, 8> ignored{
        "realmheart",
        "xwaylandvideobridge",
        "xdgdesktopportal",
        "polkitkdeauthenticationagent",
        "gcrprompter",
        "pinentry",
        "notificationdaemon",
        "hyprlandsharepicker",
    };
    return std::ranges::any_of(ignored, [&normalized](std::string_view value) {
        return normalized.find(value) != std::string::npos;
    });
}

std::string session_fallback_title(std::string_view identity) {
    std::string title(identity);
    if (title.empty()) return "Unknown application";

    const std::size_t separator = title.find_last_of("./");
    if (separator != std::string::npos && separator + 1 < title.size()) {
        title.erase(0, separator + 1);
    }
    if (!title.empty()) {
        title.front() = static_cast<char>(std::toupper(
            static_cast<unsigned char>(title.front())
        ));
    }
    return title;
}

} // namespace

std::vector<std::string> launcher_command_argv(std::string_view command) {
    const auto first = command.find_first_not_of(" \t\n\r");
    const std::string_view trimmed = first == std::string_view::npos
        ? std::string_view{}
        : command.substr(first);
    const bool needs_terminal = trimmed == "sudo" ||
        (trimmed.starts_with("sudo") && trimmed.size() > 4 &&
         std::isspace(static_cast<unsigned char>(trimmed[4])) != 0);

    std::vector<std::string> argv;
    if (needs_terminal) argv.emplace_back("kitty");
    argv.emplace_back("fish");
    argv.emplace_back("-C");
    argv.emplace_back(command);
    return argv;
}

std::vector<std::string> launcher_application_argv(std::string_view desktop_id) {
    if (desktop_id.empty()) return {};
    return {"gtk4-launch", std::string(desktop_id)};
}

std::vector<std::string> launcher_scoped_argv(const std::vector<std::string>& argv) {
    if (argv.empty() || argv.front().empty()) return {};

    std::vector<std::string> scoped{
        "systemd-run",
        "--user",
        "--scope",
        "--quiet",
        "--collect",
        "--slice=app.slice",
        "--",
    };
    scoped.insert(scoped.end(), argv.begin(), argv.end());
    return scoped;
}

std::vector<std::string> launcher_clipboard_delete_argv(
    std::string_view id,
    std::string_view preview
) {
    if (id.empty()) return {};
    return {
        "sh",
        "-c",
        "printf '%s\\t%s\\n' \"$1\" \"$2\" | cliphist delete",
        "realmheart-clipboard-delete",
        std::string(id),
        std::string(preview),
    };
}

std::vector<LauncherResult> launcher_clipboard_results(
    std::string_view cliphist_output,
    std::string_view filter,
    std::size_t limit
) {
    std::vector<LauncherResult> results;
    if (limit == 0) return results;

    std::size_t line_start = 0;
    while (line_start < cliphist_output.size() && results.size() < limit) {
        const std::size_t line_end = cliphist_output.find('\n', line_start);
        std::string_view line = cliphist_output.substr(
            line_start,
            line_end == std::string_view::npos
                ? cliphist_output.size() - line_start
                : line_end - line_start
        );
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        const std::size_t separator = line.find('\t');
        if (separator != std::string_view::npos && separator > 0) {
            const std::string_view id = line.substr(0, separator);
            const bool numeric_id = std::ranges::all_of(id, [](char character) {
                return std::isdigit(static_cast<unsigned char>(character)) != 0;
            });
            if (numeric_id) {
                const std::string_view raw_preview = line.substr(separator + 1);
                LauncherResult result;
                result.kind = LauncherResultKind::Clipboard;
                result.id = std::string(id);
                result.icon_name = "Realmheart-Icons/clip-history.svg";
                result.description = std::string(raw_preview);
                result.clipboard_mime = clipboard_image_mime(raw_preview);
                result.clipboard_image = !result.clipboard_mime.empty();

                if (result.clipboard_image) {
                    const std::string format = clipboard_format_label(result.clipboard_mime);
                    result.title = result.clipboard_mime == "image/png"
                        ? "Screenshot or copied image"
                        : "Copied image";
                    result.subtitle = "Clipboard image · " + format;
                    result.search_terms = {
                        "image", "screenshot", "picture", format, result.clipboard_mime
                    };
                } else if (clipboard_binary_preview(raw_preview)) {
                    result.title = "Binary clipboard entry";
                    result.subtitle = "Clipboard data";
                    result.search_terms = {"binary", "data", std::string(raw_preview)};
                } else {
                    result.title = realmheart::core::sanitize_command_detail(
                        raw_preview,
                        150
                    );
                    if (result.title.empty()) result.title = "Empty clipboard text";
                    result.subtitle = "Clipboard text";
                    result.search_terms = {std::string(raw_preview), "text"};
                }

                if (clipboard_filter_matches(result, filter)) {
                    results.push_back(std::move(result));
                }
            }
        }

        if (line_end == std::string_view::npos) break;
        line_start = line_end + 1;
    }
    return results;
}

std::vector<LauncherResult> launcher_emoji_results(
    std::string_view emoji_script,
    std::string_view filter,
    std::size_t limit
) {
    std::vector<LauncherResult> results;
    if (limit == 0) return results;

    constexpr std::string_view marker = "### DATA ###";
    const std::size_t data_start = data_start_after_standalone_marker(
        emoji_script,
        marker
    );
    if (data_start == std::string_view::npos) return results;

    const std::string raw_filter = trim_copy(filter);
    std::string normalized_filter = normalized_copy(raw_filter);
    std::vector<std::string> filter_terms;
    std::size_t filter_start = 0;
    while (filter_start < normalized_filter.size()) {
        const std::size_t filter_end = normalized_filter.find(' ', filter_start);
        const std::string_view term = std::string_view(normalized_filter).substr(
            filter_start,
            filter_end == std::string::npos
                ? normalized_filter.size() - filter_start
                : filter_end - filter_start
        );
        if (!term.empty()) filter_terms.emplace_back(term);
        if (filter_end == std::string::npos) break;
        filter_start = filter_end + 1;
    }

    std::size_t line_start = data_start;

    while (line_start < emoji_script.size() && results.size() < limit) {
        const std::size_t line_end = emoji_script.find('\n', line_start);
        std::string_view line = emoji_script.substr(
            line_start,
            line_end == std::string_view::npos
                ? emoji_script.size() - line_start
                : line_end - line_start
        );
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        const std::size_t first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            line.remove_prefix(first);
            const std::size_t separator = line.find_first_of(" \t");
            const std::string_view glyph = line.substr(0, separator);
            std::string_view keywords;
            if (separator != std::string_view::npos) {
                const std::size_t keyword_start = line.find_first_not_of(
                    " \t",
                    separator
                );
                if (keyword_start != std::string_view::npos) {
                    keywords = line.substr(keyword_start);
                }
            }

            if (!glyph.empty()) {
                const std::string normalized_keywords = normalized_copy(keywords);
                const bool matches = raw_filter.empty() ||
                    (filter_terms.empty()
                        ? glyph == raw_filter
                        : std::ranges::all_of(
                            filter_terms,
                            [&normalized_keywords](const std::string& term) {
                                return normalized_keywords.find(term) !=
                                    std::string::npos;
                            }
                        ));
                if (matches) {
                    LauncherResult result;
                    result.kind = LauncherResultKind::Emoji;
                    result.id = std::string(glyph);
                    result.title = keywords.empty()
                        ? std::string("Emoji")
                        : realmheart::core::sanitize_command_detail(keywords, 90);
                    result.subtitle = "Copy " + std::string(glyph) + " to clipboard";
                    result.icon_name = "Realmheart-Icons/emoji-picker.svg";
                    result.description = std::string(keywords);
                    result.search_terms = {
                        std::string(glyph),
                        std::string(keywords),
                    };
                    results.push_back(std::move(result));
                }
            }
        }

        if (line_end == std::string_view::npos) break;
        line_start = line_end + 1;
    }
    return results;
}

LauncherResult launcher_clipboard_clear_result(bool confirmation_armed) {
    LauncherResult result;
    result.kind = LauncherResultKind::ClipboardAction;
    result.id = "clear-history";
    result.title = confirmation_armed
        ? "Press Enter again to clear clipboard history"
        : "Clear clipboard history";
    result.subtitle = confirmation_armed
        ? "This permanently deletes every saved cliphist entry"
        : "Delete every saved clipboard entry";
    result.icon_name = "Realmheart-Icons/clip-history.svg";
    result.description = "Run cliphist wipe after explicit confirmation";
    return result;
}

std::vector<LauncherResult> launcher_command_suggestions(std::string_view query) {
    const std::string trimmed = trim_copy(query);
    if (trimmed.empty() || trimmed.front() != '>') return {};

    const std::string typed = lowercase_copy(trim_copy(
        std::string_view(trimmed).substr(1)
    ));
    if (typed.find_first_of(" \t\n\r") != std::string::npos) return {};

    struct CommandDefinition {
        std::string_view name;
        std::string_view subtitle;
        std::string_view description;
        std::string_view icon_name;
    };
    constexpr std::array<CommandDefinition, 3> commands{{
        {
            "clip",
            "Browse clipboard history",
            "Open Realmheart clipboard history",
            "Realmheart-Icons/clip-history.svg",
        },
        {
            "clear",
            "Clear clipboard history",
            "Delete every saved cliphist entry",
            "Realmheart-Icons/clip-history.svg",
        },
        {
            "emoji",
            "Search and copy emoji",
            "Open Realmheart emoji picker",
            "Realmheart-Icons/emoji-picker.svg",
        },
    }};

    std::vector<LauncherResult> results;
    for (const auto& command : commands) {
        if (!typed.empty() && !command.name.starts_with(typed)) continue;

        LauncherResult result;
        result.kind = LauncherResultKind::LauncherCommand;
        result.id = std::string(command.name);
        result.title = ">" + std::string(command.name);
        result.subtitle = std::string(command.subtitle);
        result.icon_name = std::string(command.icon_name);
        result.description = std::string(command.description);
        result.search_terms = {
            std::string(command.name),
            std::string(command.subtitle),
        };
        results.push_back(std::move(result));
    }
    return results;
}

bool SystemLauncherProcessExecutor::run(const std::vector<std::string>& argv) {
    if (argv.empty() || argv.front().empty()) return false;

    if (running_inside_systemd_unit() && realmheart::core::command_exists("systemd-run")) {
        return realmheart::core::run_background(launcher_scoped_argv(argv));
    }
    return realmheart::core::run_background(argv);
}

bool SystemLauncherCommandExecutor::run_command(std::string_view command) {
    const auto arguments = launcher_command_argv(command);
    SystemLauncherProcessExecutor executor;
    return executor.run(arguments);
}

LauncherService::LauncherService(
    std::unique_ptr<ILauncherCommandExecutor> command_executor,
    std::unique_ptr<ILauncherProcessExecutor> process_executor
) : command_executor_(std::move(command_executor)),
    process_executor_(std::move(process_executor)) {
    load_user_state();
    refresh_index();
    hyprland_monitor_ = std::make_unique<HyprlandApplicationMonitor>(
        [this](const HyprlandApplicationEvent& event) {
            record_hyprland_activity(event);
            session_revision_.fetch_add(1, std::memory_order_relaxed);
        }
    );
    hyprland_monitor_->start();
}

LauncherService::~LauncherService() {
    hyprland_monitor_.reset();
    save_usage_history();
}

void LauncherService::refresh_index() {
    index_.clear();
    std::unordered_set<std::string> application_identities;

    GList* apps = g_app_info_get_all();
    if (apps != nullptr) {
        for (GList* iterator = apps; iterator != nullptr; iterator = g_list_next(iterator)) {
            GAppInfo* app = static_cast<GAppInfo*>(iterator->data);
            if (!g_app_info_should_show(app)) continue;

            const char* id = g_app_info_get_id(app);
            const char* name = g_app_info_get_name(app);
            if (id == nullptr || name == nullptr) continue;

            LauncherResult result;
            result.kind = LauncherResultKind::Application;
            result.id = id;
            result.title = name;

            const char* display_name = g_app_info_get_display_name(app);
            const char* description = g_app_info_get_description(app);
            const char* executable = g_app_info_get_executable(app);
            const char* commandline = g_app_info_get_commandline(app);

            result.description = description != nullptr ? description : "";
            result.executable = executable != nullptr ? executable : "";
            if (!result.description.empty() && result.description != result.title) {
                result.subtitle = result.description;
            } else if (display_name != nullptr && result.title != display_name) {
                result.subtitle = display_name;
            } else {
                result.subtitle = result.executable;
            }

            result.search_terms.push_back(result.id);
            if (display_name != nullptr) result.search_terms.emplace_back(display_name);
            if (description != nullptr) result.search_terms.emplace_back(description);
            if (executable != nullptr) result.search_terms.emplace_back(executable);
            if (commandline != nullptr) result.search_terms.emplace_back(commandline);

            GIcon* icon = g_app_info_get_icon(app);
            if (icon != nullptr && G_TYPE_CHECK_INSTANCE_TYPE(icon, G_TYPE_THEMED_ICON)) {
                const char* const* names = g_themed_icon_get_names(G_THEMED_ICON(icon));
                if (names != nullptr && names[0] != nullptr) result.icon_name = names[0];
            }

            const std::string identity = recommendation_identity(result);
            if (!application_identities.insert(identity).second) continue;
            index_.push_back(std::move(result));
        }
        g_list_free_full(apps, g_object_unref);
    }

    const fs::path actions_path = actions_directory();
    std::error_code filesystem_error;
    if (fs::is_directory(actions_path, filesystem_error) && !filesystem_error) {
        fs::directory_iterator iterator(actions_path, filesystem_error);
        const fs::directory_iterator end;
        for (; !filesystem_error && iterator != end; iterator.increment(filesystem_error)) {
            const auto& entry = *iterator;
            if (!entry.is_regular_file(filesystem_error) || filesystem_error) continue;

            LauncherResult result;
            result.kind = LauncherResultKind::Action;
            result.id = entry.path().string();
            result.title = entry.path().stem().string();
            result.subtitle = "Realmheart action";
            result.icon_name = "system-run";
            result.description = "Custom action from ~/.config/realmheart/actions";
            result.executable = "/bin/bash " + result.id;
            result.search_terms = {result.id, entry.path().filename().string()};
            index_.push_back(std::move(result));
        }
    }
}

const LauncherResult* LauncherService::match_hyprland_application(
    std::string_view identity
) const {
    if (identity.empty()) return nullptr;

    const LauncherResult* best = nullptr;
    int best_score = 0;
    for (const auto& result : index_) {
        if (result.kind != LauncherResultKind::Application) continue;

        int score = 0;
        score = std::max(score, hyprland_identity_score(identity, result.title));
        score = std::max(score, hyprland_identity_score(
            identity,
            desktop_identity(result.id)
        ));
        score = std::max(score, hyprland_identity_score(
            identity,
            executable_identity(result.executable)
        ));
        for (const auto& term : result.search_terms) {
            score = std::max(score, hyprland_identity_score(identity, term));
        }

        if (score > best_score) {
            best_score = score;
            best = &result;
        }
    }

    // Avoid loose matches such as a short generic class accidentally mapping
    // to an unrelated desktop entry.
    return best_score >= 620 ? best : nullptr;
}

void LauncherService::set_mock_index(std::vector<LauncherResult> index) {
    index_ = std::move(index);
}

int LauncherService::calculate_score(
    const LauncherResult& result,
    std::string_view query
) const {
    const std::string normalized_query = normalized_copy(query);
    if (normalized_query.empty()) return 0;

    int score = score_field(result.title, normalized_query, 9000);
    score = std::max(score, score_field(result.subtitle, normalized_query, 4300));
    score = std::max(score, score_field(result.description, normalized_query, 3900));
    score = std::max(score, score_field(result.executable, normalized_query, 3600));
    score = std::max(score, score_field(result.id, normalized_query, 3200));
    for (const auto& term : result.search_terms) {
        score = std::max(score, score_field(term, normalized_query, 3000));
    }

    if (score <= 0) return 0;
    return score + usage_boost(result);
}

int LauncherService::usage_boost(const LauncherResult& result) const {
    int boost = 0;
    if (pin_rank(result) >= 0) boost += 900;

    std::scoped_lock lock(usage_mutex_);
    if (result.id == active_hyprland_app_id_) boost += 650;

    const auto found = usage_history_.find(result.id);
    if (found == usage_history_.end()) return boost;

    const UsageRecord& record = found->second;
    boost += static_cast<int>(
        std::min<std::uint64_t>(record.launcher_launch_count, 50) * 12
    );
    boost += static_cast<int>(
        std::min<std::uint64_t>(record.hyprland_open_count, 50) * 9
    );
    boost += static_cast<int>(
        std::min<std::uint64_t>(record.hyprland_focus_count, 120) * 5
    );

    const std::int64_t age = std::max<std::int64_t>(
        0,
        current_epoch_seconds() - record.last_used_epoch
    );
    constexpr std::int64_t hour = 60 * 60;
    constexpr std::int64_t day = 24 * hour;
    if (age <= hour) boost += 700;
    else if (age <= day) boost += 480;
    else if (age <= 7 * day) boost += 260;
    else if (age <= 30 * day) boost += 110;
    return boost;
}

int LauncherService::pin_rank(const LauncherResult& result) const {
    const std::array<std::string, 3> identities{
        lowercase_copy(result.id),
        lowercase_copy(result.title),
        lowercase_copy(result.executable),
    };

    for (std::size_t index = 0; index < pinned_entries_.size(); ++index) {
        const std::string& pin = pinned_entries_[index];
        if (std::find(identities.begin(), identities.end(), pin) != identities.end()) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

std::vector<LauncherResult> LauncherService::recommendations(std::size_t limit) const {
    if (limit == 0) return {};

    struct Candidate {
        const LauncherResult* result = nullptr;
        int pin = -1;
        int score = 0;
    };

    std::vector<Candidate> candidates;
    std::unordered_set<std::string> seen;
    for (const auto& item : index_) {
        if (item.kind != LauncherResultKind::Application &&
            item.kind != LauncherResultKind::Action) {
            continue;
        }
        if (!seen.insert(recommendation_identity(item)).second) continue;
        candidates.push_back({&item, pin_rank(item), usage_boost(item)});
    }

    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        const bool left_pinned = left.pin >= 0;
        const bool right_pinned = right.pin >= 0;
        if (left_pinned != right_pinned) return left_pinned;
        if (left_pinned && left.pin != right.pin) return left.pin < right.pin;
        if (left.score != right.score) return left.score > right.score;
        if (left.result->kind != right.result->kind) {
            return left.result->kind == LauncherResultKind::Application;
        }
        const std::string left_title = lowercase_copy(left.result->title);
        const std::string right_title = lowercase_copy(right.result->title);
        if (left_title != right_title) return left_title < right_title;
        return left.result->id < right.result->id;
    });

    std::vector<LauncherResult> results;
    results.reserve(std::min(limit, candidates.size()));
    for (const auto& candidate : candidates) {
        if (results.size() >= limit) break;
        results.push_back(*candidate.result);
    }
    return results;
}

std::optional<LauncherResult> LauncherService::application_by_id(std::string_view id) const {
    const auto iterator = std::find_if(index_.begin(), index_.end(), [id](const LauncherResult& result) {
        return result.kind == LauncherResultKind::Application && result.id == id;
    });
    if (iterator == index_.end()) return std::nullopt;
    return *iterator;
}

std::vector<LauncherSessionApplication> LauncherService::session_applications(
    std::size_t limit
) const {
    if (limit == 0) return {};

    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::milliseconds(650);
    options.max_output_bytes = 1024 * 1024;
    const HyprlandSessionSnapshot snapshot = HyprlandSession::read(options);
    if (!snapshot.available) return {};

    std::vector<LauncherSessionApplication> groups;
    std::unordered_map<std::string, std::size_t> group_by_identity;
    groups.reserve(std::min(limit, snapshot.windows.size()));

    for (const auto& window : snapshot.windows) {
        if (ignored_session_identity(window.app_id)) continue;

        const LauncherResult* matched = match_hyprland_application(window.app_id);
        LauncherResult application;
        std::string identity;
        if (matched != nullptr) {
            application = *matched;
            identity = application.id;
        } else {
            identity = "hyprland:" + compact_identity(window.app_id);
            application.kind = LauncherResultKind::Application;
            application.id = identity;
            application.title = session_fallback_title(window.app_id);
            application.subtitle = window.app_id;
            application.icon_name = "application-x-executable";
            application.description = "Running Hyprland application";
            application.executable = window.app_id;
            application.search_terms = {window.app_id};
        }

        auto found = group_by_identity.find(identity);
        if (found == group_by_identity.end()) {
            LauncherSessionApplication group;
            group.application = std::move(application);
            group.active = window.active;
            group.focus_rank = window.focus_history_id;
            groups.push_back(std::move(group));
            found = group_by_identity.emplace(identity, groups.size() - 1).first;
        }

        LauncherSessionApplication& group = groups[found->second];
        group.active = group.active || window.active;
        group.focus_rank = std::min(group.focus_rank, window.focus_history_id);
        group.windows.push_back({
            window.address,
            window.title,
            window.workspace_id,
            window.active,
        });
    }

    std::stable_sort(groups.begin(), groups.end(), [](const auto& left, const auto& right) {
        if (left.active != right.active) return left.active;
        if (left.focus_rank != right.focus_rank) return left.focus_rank < right.focus_rank;
        return left.application.title < right.application.title;
    });
    if (groups.size() > limit) groups.resize(limit);
    return groups;
}

bool LauncherService::focus_window(std::string_view address) const {
    realmheart::core::CommandOptions options;
    options.deadline = std::chrono::milliseconds(650);
    options.max_output_bytes = 8 * 1024;
    return HyprlandSession::focus_window(address, options);
}

std::uint64_t LauncherService::session_revision() const noexcept {
    return session_revision_.load(std::memory_order_relaxed);
}

std::vector<LauncherResult> LauncherService::search(
    std::string_view query,
    std::size_t limit
) const {
    std::vector<LauncherResult> results;
    if (limit == 0) return results;

    const std::string trimmed = trim_copy(query);
    if (trimmed.empty()) return results;

    const bool explicit_command = trimmed.front() == '>' || trimmed.front() == '$';
    std::string searchable = trimmed;
    if (explicit_command) {
        searchable = trim_copy(std::string_view(trimmed).substr(1));
        if (searchable.empty()) return results;
    }

    LauncherResult command_result;
    command_result.kind = LauncherResultKind::Command;
    command_result.id = searchable;
    command_result.title = explicit_command ? "Run explicit command" : "Run command";
    command_result.subtitle = searchable;
    command_result.icon_name = "Realmheart-Icons/run-command.svg";
    command_result.description = "Execute this input through fish";
    command_result.executable = first_command_token(searchable);

    if (explicit_command) {
        results.push_back(std::move(command_result));
        return results;
    }

    const auto calculation = evaluate_calculation(searchable);

    std::vector<ScoredResult> scored;
    for (std::size_t index = 0; index < index_.size(); ++index) {
        const int score = calculate_score(index_[index], searchable);
        if (score > 0) scored.push_back({index, score});
    }
    std::stable_sort(scored.begin(), scored.end(), [this](const auto& left, const auto& right) {
        if (left.score != right.score) return left.score > right.score;
        const auto& left_result = index_[left.index];
        const auto& right_result = index_[right.index];
        const std::string left_title = lowercase_copy(left_result.title);
        const std::string right_title = lowercase_copy(right_result.title);
        if (left_title != right_title) return left_title < right_title;
        return left_result.id < right_result.id;
    });

    std::size_t utility_slots = 0;
    if (calculation.has_value()) {
        utility_slots = std::min<std::size_t>(limit, 2);
    } else if (limit >= 2 || scored.empty()) {
        utility_slots = 1;
    }
    const std::size_t searchable_slots = limit - utility_slots;

    std::unordered_set<std::string> seen;
    for (const auto& item : scored) {
        if (results.size() >= searchable_slots) break;
        const LauncherResult& result = index_[item.index];
        if (!seen.insert(recommendation_identity(result)).second) continue;
        results.push_back(result);
    }

    if (calculation.has_value() && results.size() < limit) {
        LauncherResult calculation_result;
        calculation_result.kind = LauncherResultKind::Calculation;
        calculation_result.id = calculation->result;
        calculation_result.title = "Calculate";
        calculation_result.subtitle = calculation->display_expression +
            " = " + calculation->result;
        calculation_result.icon_name = "Realmheart-Icons/calculate.svg";
        calculation_result.description = "Copy the result to the clipboard";
        results.push_back(std::move(calculation_result));
    }

    if (results.size() < limit) {
        results.push_back(std::move(command_result));
    }
    return results;
}

bool LauncherService::activate(const LauncherResult& result) {
    bool activated = false;
    if (result.kind == LauncherResultKind::Application) {
        if (result.id.empty()) return false;
        activated = process_executor_->run(launcher_application_argv(result.id));
    } else if (result.kind == LauncherResultKind::Command) {
        if (result.id.find_first_not_of(" \t\n\r") == std::string::npos) return false;
        activated = command_executor_->run_command(result.id);
    } else if (result.kind == LauncherResultKind::Calculation) {
        if (result.id.empty()) return false;
        activated = process_executor_->run({"wl-copy", result.id});
    } else if (result.kind == LauncherResultKind::Action) {
        if (result.id.empty()) return false;
        activated = process_executor_->run({"/bin/bash", result.id});
    } else if (result.kind == LauncherResultKind::Emoji) {
        if (result.id.empty()) return false;
        activated = process_executor_->run({"wl-copy", result.id});
    } else if (result.kind == LauncherResultKind::Clipboard) {
        if (result.id.empty()) return false;
        if (result.clipboard_image && !result.clipboard_mime.empty()) {
            activated = process_executor_->run({
                "sh",
                "-c",
                "cliphist decode \"$1\" | wl-copy --type \"$2\"",
                "realmheart-clipboard",
                result.id,
                result.clipboard_mime,
            });
        } else {
            activated = process_executor_->run({
                "sh",
                "-c",
                "cliphist decode \"$1\" | wl-copy",
                "realmheart-clipboard",
                result.id,
            });
        }
    }

    if (activated && (result.kind == LauncherResultKind::Application ||
                      result.kind == LauncherResultKind::Action)) {
        record_activation(result);
    }
    return activated;
}

void LauncherService::load_user_state() {
    pinned_entries_.clear();
    std::ifstream pins(launcher_pins_path());
    std::string line;
    while (std::getline(pins, line)) {
        const std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.front() == '#') continue;
        pinned_entries_.push_back(lowercase_copy(trimmed));
    }

    std::unordered_map<std::string, UsageRecord> loaded;
    std::ifstream history(launcher_history_path());
    while (std::getline(history, line)) {
        const auto fields = split_tabs(line);
        try {
            UsageRecord record;
            std::string id;

            if (fields.size() == 3) {
                // v0.7 compatibility: launcher_count, last_launch, desktop_id
                record.launcher_launch_count = std::stoull(fields[0]);
                record.last_used_epoch = std::stoll(fields[1]);
                id = fields[2];
            } else if (fields.size() == 5) {
                record.launcher_launch_count = std::stoull(fields[0]);
                record.hyprland_focus_count = std::stoull(fields[1]);
                record.hyprland_open_count = std::stoull(fields[2]);
                record.last_used_epoch = std::stoll(fields[3]);
                id = fields[4];
            } else {
                continue;
            }

            if (!id.empty()) loaded[id] = record;
        } catch (...) {
            // A malformed line must not prevent the launcher from opening.
        }
    }

    std::scoped_lock lock(usage_mutex_);
    usage_history_ = std::move(loaded);
}

void LauncherService::save_usage_history() const {
    std::scoped_lock file_lock(history_file_mutex_);

    std::unordered_map<std::string, UsageRecord> snapshot;
    {
        std::scoped_lock lock(usage_mutex_);
        if (!usage_dirty_) return;
        snapshot = usage_history_;
        usage_dirty_ = false;
    }

    const fs::path path = launcher_history_path();
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        std::scoped_lock lock(usage_mutex_);
        usage_dirty_ = true;
        return;
    }

    const fs::path temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        std::scoped_lock lock(usage_mutex_);
        usage_dirty_ = true;
        return;
    }
    for (const auto& [id, record] : snapshot) {
        output << record.launcher_launch_count << '\t'
               << record.hyprland_focus_count << '\t'
               << record.hyprland_open_count << '\t'
               << record.last_used_epoch << '\t'
               << id << '\n';
    }
    output.close();
    if (!output) {
        std::scoped_lock lock(usage_mutex_);
        usage_dirty_ = true;
        return;
    }

    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
    }
    if (error) {
        std::scoped_lock lock(usage_mutex_);
        usage_dirty_ = true;
    }
}

void LauncherService::record_activation(const LauncherResult& result) {
    {
        std::scoped_lock lock(usage_mutex_);
        UsageRecord& record = usage_history_[result.id];
        ++record.launcher_launch_count;
        record.last_used_epoch = current_epoch_seconds();
        usage_dirty_ = true;
    }
    save_usage_history();
}

void LauncherService::record_hyprland_activity(
    const HyprlandApplicationEvent& event
) {
    if (event.kind == HyprlandApplicationEventKind::ContextChanged) return;

    const LauncherResult* result = match_hyprland_application(event.app_identity);
    if (result == nullptr) {
        if (event.kind == HyprlandApplicationEventKind::Focused) {
            std::scoped_lock lock(usage_mutex_);
            active_hyprland_app_id_.clear();
        }
        return;
    }

    const std::int64_t now = current_epoch_seconds();
    bool should_save = false;
    {
        std::scoped_lock lock(usage_mutex_);

        if (event.kind == HyprlandApplicationEventKind::Focused) {
            const std::string normalized = normalized_copy(event.app_identity);
            // Hyprland may repeat the same active-window class during quick
            // title or state changes. Count a sustained focus at most once per
            // minute, while still counting real switches immediately.
            if (normalized == last_focused_hyprland_identity_ &&
                now - last_focus_epoch_ < 60) {
                return;
            }
            last_focused_hyprland_identity_ = normalized;
            last_focus_epoch_ = now;
            active_hyprland_app_id_ = result->id;
        }

        UsageRecord& record = usage_history_[result->id];
        if (event.kind == HyprlandApplicationEventKind::Opened) {
            ++record.hyprland_open_count;
        } else {
            ++record.hyprland_focus_count;
        }
        record.last_used_epoch = now;
        usage_dirty_ = true;

        ++unsaved_hyprland_events_;
        if (unsaved_hyprland_events_ >= 12) {
            unsaved_hyprland_events_ = 0;
            should_save = true;
        }
    }

    if (should_save) save_usage_history();
}

} // namespace realmheart::services
