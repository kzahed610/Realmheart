#include "WindowEffectConfig.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

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

WindowEffectPool allRegisteredEffects() {
    WindowEffectPool effects;
    for (const auto& effect : windowEffectSpecs()) {
        if (!windowEffectIsNone(effect))
            effects.push_back(effect.name);
    }
    return effects;
}

bool appendEffect(
    std::string effectName,
    WindowEffectPool& effects,
    std::string& error
) {
    if (effectName == "@all") {
        error = "@all must be used by itself, not inside an effect array";
        return false;
    }
    if (findWindowEffect(effectName) == nullptr) {
        error = "unknown registered effect: " + effectName;
        return false;
    }
    if (std::find(effects.begin(), effects.end(), effectName) != effects.end()) {
        error = "duplicate effect in pool: " + effectName;
        return false;
    }

    effects.push_back(std::move(effectName));
    return true;
}

std::optional<WindowEffectPool> parseEffectPool(
    std::string_view value,
    std::string& error
) {
    value = trim(value);
    if (value.empty()) {
        error = "expected an effect string or array";
        return std::nullopt;
    }

    if (value.front() == '"' || value.front() == '\'') {
        std::string effectName;
        if (!parseQuotedString(value, effectName, error))
            return std::nullopt;

        if (effectName == "@all") {
            auto effects = allRegisteredEffects();
            if (effects.empty()) {
                error = "@all resolved to no renderable effects";
                return std::nullopt;
            }
            return effects;
        }

        WindowEffectPool effects;
        if (!appendEffect(std::move(effectName), effects, error))
            return std::nullopt;
        return effects;
    }

    if (value.front() != '[' || value.back() != ']') {
        error = "expected a quoted effect, @all, or a one-line TOML string array";
        return std::nullopt;
    }

    value = trim(value.substr(1U, value.size() - 2U));
    if (value.empty()) {
        error = "effect pool cannot be empty";
        return std::nullopt;
    }

    WindowEffectPool effects;
    std::size_t offset = 0U;
    while (offset < value.size()) {
        while (offset < value.size() &&
               (value[offset] == ' ' || value[offset] == '\t')) {
            ++offset;
        }
        if (offset >= value.size())
            break;

        const char quote = value[offset];
        if (quote != '"' && quote != '\'') {
            error = "effect arrays may contain only quoted strings";
            return std::nullopt;
        }

        const std::size_t tokenStart = offset;
        ++offset;
        bool escaped = false;
        bool closed = false;
        while (offset < value.size()) {
            const char current = value[offset++];
            if (quote == '"' && !escaped && current == '\\') {
                escaped = true;
                continue;
            }
            if (!escaped && current == quote) {
                closed = true;
                break;
            }
            escaped = false;
        }

        if (!closed) {
            error = "unterminated quoted effect in array";
            return std::nullopt;
        }

        std::string effectName;
        if (!parseQuotedString(
                value.substr(tokenStart, offset - tokenStart),
                effectName,
                error
            )) {
            return std::nullopt;
        }
        if (!appendEffect(std::move(effectName), effects, error))
            return std::nullopt;

        while (offset < value.size() &&
               (value[offset] == ' ' || value[offset] == '\t')) {
            ++offset;
        }
        if (offset >= value.size())
            break;
        if (value[offset] != ',') {
            error = "expected a comma between effects";
            return std::nullopt;
        }
        ++offset;

        while (offset < value.size() &&
               (value[offset] == ' ' || value[offset] == '\t')) {
            ++offset;
        }
        if (offset >= value.size())
            break; // TOML permits a trailing comma.
    }

    if (effects.empty()) {
        error = "effect pool cannot be empty";
        return std::nullopt;
    }
    return effects;
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
    if (!rule.openEffects && !rule.closeEffects) {
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
    auto effects = allRegisteredEffects();
    if (effects.empty())
        effects.push_back(std::string{kNoWindowEffect});

    config.defaultOpenEffects = effects;
    config.defaultCloseEffects = std::move(effects);
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

std::filesystem::path defaultWindowEffectAssetRoot() {
    // 1. Explicit override wins.
    if (const char* overrideRoot = std::getenv("REALMHEART_EFFECTS_DIR");
        overrideRoot != nullptr && *overrideRoot != '\0') {
        return overrideRoot;
    }

    // 2. Discover the loaded .so's own directory from /proc/self/maps and look
    //    for the effects directory relative to it. This makes the plugin work
    //    when the repo (and its build tree) is copied to a fresh account,
    //    instead of baking-in the original absolute source path.
    std::ifstream maps("/proc/self/maps");
    std::string line;
    std::vector<std::filesystem::path> selfDirs;
    while (maps && std::getline(maps, line)) {
        // Look for a mapping of realmheart-fx.so.
        const auto marker = line.find("realmheart-fx.so");
        if (marker == std::string::npos) continue;

        const auto pathStart = line.find('/', marker > 3 ? marker - 3 : 0);
        if (pathStart == std::string::npos) continue;
        const auto pathEnd = line.find('\n', pathStart);
        const std::string mappedPath = line.substr(
            pathStart,
            pathEnd == std::string::npos ? std::string::npos : pathEnd - pathStart
        );
        if (mappedPath.empty()) continue;

        std::filesystem::path soPath{mappedPath};
        const auto soDir = soPath.parent_path();
        if (soDir.empty()) continue;

        // Candidates relative to the .so directory, in preference order.
        for (const auto& candidate : {
                 soDir / "effects" / "windows",
                 soDir / ".." / "effects" / "windows",
                 soDir / ".." / "realmheart" / "effects" / "windows",
             }) {
            std::error_code error;
            if (std::filesystem::is_directory(candidate, error) && !error) {
                return candidate;
            }
        }
    }

    // 3. Compile-time constant as a last resort (original behaviour). The
    //    macro name varies by target (plugin vs test vs GL probe); any one that
    //    is defined becomes the fallback.
#if defined(REALMHEART_WINDOW_EFFECT_ASSET_DIR)
    return REALMHEART_WINDOW_EFFECT_ASSET_DIR;
#elif defined(REALMHEART_TEST_WINDOW_EFFECT_DIR)
    return REALMHEART_TEST_WINDOW_EFFECT_DIR;
#elif defined(REALMHEART_SOURCE_WINDOW_EFFECT_DIR)
    return REALMHEART_SOURCE_WINDOW_EFFECT_DIR;
#else
    return {};
#endif
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

    SWindowEffectConfig parsed = builtInWindowEffectConfig();
    parsed.rules.clear();
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
                const auto effects = parseEffectPool(value, parseError);
                if (!effects) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                parsed.defaultOpenEffects = *effects;
            } else if (key == "close") {
                const auto effects = parseEffectPool(value, parseError);
                if (!effects) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                parsed.defaultCloseEffects = *effects;
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
                const auto effects = parseEffectPool(value, parseError);
                if (!effects) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                currentRule->openEffects = *effects;
            } else if (key == "close") {
                const auto effects = parseEffectPool(value, parseError);
                if (!effects) {
                    result.error = loadError(lineNumber, parseError);
                    return result;
                }
                currentRule->closeEffects = *effects;
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

std::string windowEffectPoolSummary(const WindowEffectPool& pool) {
    if (pool.empty())
        return "none";
    if (pool.size() == 1U)
        return pool.front();

    std::ostringstream summary;
    summary << '[';
    for (std::size_t index = 0U; index < pool.size(); ++index) {
        if (index != 0U)
            summary << ',';
        summary << pool[index];
    }
    summary << ']';
    return summary.str();
}

std::string windowEffectConfigSummary(
    const SWindowEffectConfig& config,
    const std::filesystem::path& requestedPath
) {
    std::ostringstream summary;
    summary << "source=" << (config.loadedFromFile ? "file" : "built-in")
            << " path=" << (requestedPath.empty() ? "<unavailable>" : requestedPath.string())
            << " open=" << windowEffectPoolSummary(config.defaultOpenEffects)
            << " close=" << windowEffectPoolSummary(config.defaultCloseEffects)
            << " rules=" << config.rules.size();
    return summary.str();
}
