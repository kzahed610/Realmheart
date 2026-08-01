#include "WindowEffectConfig.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace {

enum class EConfigSection {
    None,
    Windows,
    WindowRule,
};

std::string_view trim(std::string_view value) noexcept {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

std::string_view stripComment(std::string_view line) noexcept {
    bool inBasicString = false;
    bool inLiteralString = false;
    bool escaped = false;

    for (std::size_t index = 0; index < line.size(); ++index) {
        const char current = line[index];

        if (inBasicString) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (current == '\\') {
                escaped = true;
                continue;
            }
            if (current == '"')
                inBasicString = false;
            continue;
        }

        if (inLiteralString) {
            if (current == '\'')
                inLiteralString = false;
            continue;
        }

        if (current == '"') {
            inBasicString = true;
            continue;
        }
        if (current == '\'') {
            inLiteralString = true;
            continue;
        }
        if (current == '#')
            return line.substr(0, index);
    }

    return line;
}

bool parseQuotedString(
    std::string_view value,
    std::string& output,
    std::string& error
) {
    value = trim(value);
    if (value.size() < 2U ||
        !((value.front() == '"' && value.back() == '"') ||
          (value.front() == '\'' && value.back() == '\''))) {
        error = "expected a quoted TOML string";
        return false;
    }

    const char quote = value.front();
    output.clear();
    output.reserve(value.size() - 2U);

    if (quote == '\'') {
        output.assign(value.substr(1U, value.size() - 2U));
        return true;
    }

    bool escaped = false;
    for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
        const char current = value[index];
        if (!escaped) {
            if (current == '\\') {
                escaped = true;
                continue;
            }
            output.push_back(current);
            continue;
        }

        escaped = false;
        switch (current) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            default:
                error = std::string{"unsupported escape sequence: \\"} + current;
                return false;
        }
    }

    if (escaped) {
        error = "unfinished escape sequence";
        return false;
    }

    return true;
}

const SWindowEffectSpec* parseEffect(
    std::string_view value,
    std::string& error
) {
    std::string effectName;
    if (!parseQuotedString(value, effectName, error))
        return nullptr;

    const auto* effect = findWindowEffect(effectName);
    if (effect == nullptr)
        error = "unknown registered effect: " + effectName;
    return effect;
}

std::optional<EWindowEffectTextMatch> parseMatchMode(
    std::string_view value,
    std::string& error
) {
    std::string mode;
    if (!parseQuotedString(value, mode, error))
        return std::nullopt;

    if (mode == "exact")
        return EWindowEffectTextMatch::Exact;
    if (mode == "prefix")
        return EWindowEffectTextMatch::Prefix;
    if (mode == "contains")
        return EWindowEffectTextMatch::Contains;

    error = "match mode must be exact, prefix, or contains";
    return std::nullopt;
}

bool validateRule(const SWindowEffectRule& rule, std::string& error) {
    if (!rule.windowClass && !rule.windowTitle) {
        error = "a [[windows.rules]] entry needs class and/or title";
        return false;
    }
    if (!rule.openEffect && !rule.closeEffect) {
        error = "a [[windows.rules]] entry needs open and/or close";
        return false;
    }
    return true;
}

std::string loadError(
    std::size_t line,
    const std::string& message
) {
    return "line " + std::to_string(line) + ": " + message;
}

} // namespace

SWindowEffectConfig builtInWindowEffectConfig() {
    SWindowEffectConfig config;
    config.rules.push_back({
        .windowClass = std::string{"kitty"},
        .classMatch = EWindowEffectTextMatch::Exact,
        .windowTitle = std::nullopt,
        .titleMatch = EWindowEffectTextMatch::Contains,
        .openEffect = EWindowEffectId::AetherSunder,
        .closeEffect = EWindowEffectId::AetherSunder,
    });
    return config;
}

std::filesystem::path defaultWindowEffectConfigPath() {
    if (const char* overridePath = std::getenv("REALMHEART_FX_CONFIG");
        overridePath != nullptr && *overridePath != '\0') {
        return overridePath;
    }

    if (const char* xdgConfig = std::getenv("XDG_CONFIG_HOME");
        xdgConfig != nullptr && *xdgConfig != '\0') {
        return std::filesystem::path{xdgConfig} /
            "realmheart" / "window-effects.toml";
    }

    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home} /
            ".config" / "realmheart" / "window-effects.toml";
    }

    return {};
}

SWindowEffectConfigLoadResult loadWindowEffectConfig(
    const std::filesystem::path& path
) {
    SWindowEffectConfigLoadResult result;
    result.config = builtInWindowEffectConfig();
    result.config.sourcePath = path;

    std::ifstream stream(path);
    if (!stream) {
        result.error = "could not open " + path.string();
        return result;
    }

    SWindowEffectConfig parsed;
    parsed.sourcePath = path;
    parsed.loadedFromFile = true;

    EConfigSection section = EConfigSection::None;
    SWindowEffectRule* currentRule = nullptr;
    std::unordered_set<std::string> currentKeys;
    std::string rawLine;
    std::size_t lineNumber = 0U;

    while (std::getline(stream, rawLine)) {
        ++lineNumber;
        const std::string_view line = trim(stripComment(rawLine));
        if (line.empty())
            continue;

        if (line == "[windows]") {
            if (currentRule != nullptr) {
                std::string validationError;
                if (!validateRule(*currentRule, validationError)) {
                    result.error = loadError(lineNumber, validationError);
                    return result;
                }
            }
            section = EConfigSection::Windows;
            currentRule = nullptr;
            currentKeys.clear();
            continue;
        }

        if (line == "[[windows.rules]]") {
            if (currentRule != nullptr) {
                std::string validationError;
                if (!validateRule(*currentRule, validationError)) {
                    result.error = loadError(lineNumber, validationError);
                    return result;
                }
            }
            section = EConfigSection::WindowRule;
            parsed.rules.emplace_back();
            currentRule = &parsed.rules.back();
            currentKeys.clear();
            continue;
        }

        if (!line.empty() && line.front() == '[') {
            result.error = loadError(lineNumber, "unsupported TOML table: " + std::string(line));
            return result;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            result.error = loadError(lineNumber, "expected key = value");
            return result;
        }

        const std::string key{trim(line.substr(0U, equals))};
        const std::string_view value = trim(line.substr(equals + 1U));
        if (key.empty() || value.empty()) {
            result.error = loadError(lineNumber, "expected non-empty key and value");
            return result;
        }
        if (!currentKeys.insert(key).second) {
            result.error = loadError(lineNumber, "duplicate key in table: " + key);
            return result;
        }

        std::string parseError;
        if (section == EConfigSection::Windows) {
            if (key == "open") {
                const auto* effect = parseEffect(value, parseError);
                if (effect == nullptr) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                parsed.defaultOpenEffect = effect->id;
            } else if (key == "close") {
                const auto* effect = parseEffect(value, parseError);
                if (effect == nullptr) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                parsed.defaultCloseEffect = effect->id;
            } else {
                result.error = loadError(lineNumber, "unsupported [windows] key: " + key);
                return result;
            }
            continue;
        }

        if (section == EConfigSection::WindowRule && currentRule != nullptr) {
            if (key == "class") {
                std::string pattern;
                if (!parseQuotedString(value, pattern, parseError)) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                if (pattern.empty()) {
                    result.error = loadError(lineNumber, "class pattern cannot be empty");
                    return result;
                }
                currentRule->windowClass = std::move(pattern);
            } else if (key == "class_match") {
                const auto match = parseMatchMode(value, parseError);
                if (!match) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                currentRule->classMatch = *match;
            } else if (key == "title") {
                std::string pattern;
                if (!parseQuotedString(value, pattern, parseError)) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                if (pattern.empty()) {
                    result.error = loadError(lineNumber, "title pattern cannot be empty");
                    return result;
                }
                currentRule->windowTitle = std::move(pattern);
            } else if (key == "title_match") {
                const auto match = parseMatchMode(value, parseError);
                if (!match) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                currentRule->titleMatch = *match;
            } else if (key == "open") {
                const auto* effect = parseEffect(value, parseError);
                if (effect == nullptr) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                currentRule->openEffect = effect->id;
            } else if (key == "close") {
                const auto* effect = parseEffect(value, parseError);
                if (effect == nullptr) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                currentRule->closeEffect = effect->id;
            } else {
                result.error = loadError(lineNumber, "unsupported [[windows.rules]] key: " + key);
                return result;
            }
            continue;
        }

        result.error = loadError(lineNumber, "key appears outside [windows] or [[windows.rules]]");
        return result;
    }

    if (currentRule != nullptr) {
        std::string validationError;
        if (!validateRule(*currentRule, validationError)) {
            result.error = loadError(lineNumber, validationError);
            return result;
        }
    }

    result.success = true;
    result.config = std::move(parsed);
    return result;
}

std::string windowEffectConfigSummary(
    const SWindowEffectConfig& config,
    const std::filesystem::path& requestedPath
) {
    const auto* open = findWindowEffect(config.defaultOpenEffect);
    const auto* close = findWindowEffect(config.defaultCloseEffect);

    std::ostringstream summary;
    summary << "source=" << (config.loadedFromFile ? "file" : "built-in")
            << " path=" << (requestedPath.empty() ? "<unavailable>" : requestedPath.string())
            << " open=" << (open ? open->name : "missing")
            << " close=" << (close ? close->name : "missing")
            << " rules=" << config.rules.size();
    return summary.str();
}
