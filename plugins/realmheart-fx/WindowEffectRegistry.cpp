#include "WindowEffectRegistry.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace {

constexpr WindowEffectCapabilityMask kTexturedWindowCapabilities =
    windowEffectCapabilityBit(EWindowEffectCapability::SourceTexture) |
    windowEffectCapabilityBit(EWindowEffectCapability::Texture2D) |
    windowEffectCapabilityBit(EWindowEffectCapability::ExternalTexture) |
    windowEffectCapabilityBit(EWindowEffectCapability::RoundedSource);

std::vector<SWindowEffectSpec> g_windowEffectSpecs{{
    .name = std::string{kNoWindowEffect},
    .displayName = "None",
    .fragmentShaderAsset = {},
    .openDurationSeconds = 0.0F,
    .closeDurationSeconds = 0.0F,
    .reversible = true,
    .capabilities = 0U,
}};

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
            return line.substr(0U, index);
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

bool parseBoolean(std::string_view value, bool& output, std::string& error) {
    value = trim(value);
    if (value == "true") {
        output = true;
        return true;
    }
    if (value == "false") {
        output = false;
        return true;
    }
    error = "expected true or false";
    return false;
}

bool parsePositiveFloat(std::string_view value, float& output, std::string& error) {
    value = trim(value);
    if (value.empty()) {
        error = "expected a positive number";
        return false;
    }

    std::string storage{value};
    char* end = nullptr;
    const float parsed = std::strtof(storage.c_str(), &end);
    if (end == storage.c_str() || *end != '\0' || !std::isfinite(parsed) || parsed <= 0.0F) {
        error = "expected a finite positive number";
        return false;
    }

    output = parsed;
    return true;
}

bool validEffectName(std::string_view name) noexcept {
    if (name.empty() || name == kNoWindowEffect)
        return false;

    for (const char character : name) {
        const bool valid =
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '-';
        if (!valid)
            return false;
    }

    return name.front() != '-' && name.back() != '-';
}

bool safeRelativePath(const std::filesystem::path& path) noexcept {
    if (path.empty() || path.is_absolute())
        return false;

    for (const auto& component : path) {
        if (component == "..")
            return false;
    }
    return true;
}

std::string manifestError(
    const std::filesystem::path& manifest,
    std::size_t line,
    const std::string& message
) {
    std::ostringstream output;
    output << manifest.string();
    if (line != 0U)
        output << ':' << line;
    output << ": " << message;
    return output.str();
}

struct SParsedManifest {
    SWindowEffectSpec spec;
    bool hasName = false;
    bool hasShader = false;
    bool hasOpenDuration = false;
    bool hasCloseDuration = false;
};

bool parseManifest(
    const std::filesystem::path& effectRoot,
    const std::filesystem::path& manifest,
    SWindowEffectSpec& output,
    std::string& error
) {
    std::ifstream stream(manifest);
    if (!stream) {
        error = manifestError(manifest, 0U, "could not open effect manifest");
        return false;
    }

    enum class ESection {
        None,
        Effect,
        Capabilities,
    };

    SParsedManifest parsed;
    parsed.spec.reversible = true;
    parsed.spec.capabilities = kTexturedWindowCapabilities;

    ESection section = ESection::None;
    std::unordered_set<std::string> effectKeys;
    std::unordered_set<std::string> capabilityKeys;
    std::string rawLine;
    std::size_t lineNumber = 0U;

    while (std::getline(stream, rawLine)) {
        ++lineNumber;
        const std::string_view line = trim(stripComment(rawLine));
        if (line.empty())
            continue;

        if (line == "[effect]") {
            section = ESection::Effect;
            continue;
        }
        if (line == "[capabilities]") {
            section = ESection::Capabilities;
            continue;
        }
        if (line.front() == '[') {
            error = manifestError(manifest, lineNumber, "unsupported TOML table: " + std::string(line));
            return false;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            error = manifestError(manifest, lineNumber, "expected key = value");
            return false;
        }

        const std::string key{trim(line.substr(0U, equals))};
        const std::string_view value = trim(line.substr(equals + 1U));
        if (key.empty() || value.empty()) {
            error = manifestError(manifest, lineNumber, "expected non-empty key and value");
            return false;
        }

        auto& keys = section == ESection::Capabilities ? capabilityKeys : effectKeys;
        if (!keys.insert(key).second) {
            error = manifestError(manifest, lineNumber, "duplicate key in table: " + key);
            return false;
        }

        std::string parseError;
        if (section == ESection::Effect) {
            if (key == "id") {
                if (!parseQuotedString(value, parsed.spec.name, parseError)) {
                    error = manifestError(manifest, lineNumber, parseError);
                    return false;
                }
                if (!validEffectName(parsed.spec.name)) {
                    error = manifestError(
                        manifest,
                        lineNumber,
                        "id must use lowercase letters, digits, and hyphens; 'none' is reserved"
                    );
                    return false;
                }
                parsed.hasName = true;
            } else if (key == "display_name") {
                if (!parseQuotedString(value, parsed.spec.displayName, parseError)) {
                    error = manifestError(manifest, lineNumber, parseError);
                    return false;
                }
                if (parsed.spec.displayName.empty()) {
                    error = manifestError(manifest, lineNumber, "display_name cannot be empty");
                    return false;
                }
            } else if (key == "shader") {
                std::string shader;
                if (!parseQuotedString(value, shader, parseError)) {
                    error = manifestError(manifest, lineNumber, parseError);
                    return false;
                }
                const std::filesystem::path relativeShader{shader};
                if (!safeRelativePath(relativeShader)) {
                    error = manifestError(manifest, lineNumber, "shader must be a safe relative path");
                    return false;
                }
                parsed.spec.fragmentShaderAsset = manifest.parent_path().filename() / relativeShader;
                parsed.hasShader = true;
            } else if (key == "open_duration") {
                if (!parsePositiveFloat(value, parsed.spec.openDurationSeconds, parseError)) {
                    error = manifestError(manifest, lineNumber, parseError);
                    return false;
                }
                parsed.hasOpenDuration = true;
            } else if (key == "close_duration") {
                if (!parsePositiveFloat(value, parsed.spec.closeDurationSeconds, parseError)) {
                    error = manifestError(manifest, lineNumber, parseError);
                    return false;
                }
                parsed.hasCloseDuration = true;
            } else if (key == "reversible") {
                if (!parseBoolean(value, parsed.spec.reversible, parseError)) {
                    error = manifestError(manifest, lineNumber, parseError);
                    return false;
                }
            } else {
                error = manifestError(manifest, lineNumber, "unsupported [effect] key: " + key);
                return false;
            }
            continue;
        }

        if (section == ESection::Capabilities) {
            EWindowEffectCapability capability;
            if (key == "source_texture")
                capability = EWindowEffectCapability::SourceTexture;
            else if (key == "texture_2d")
                capability = EWindowEffectCapability::Texture2D;
            else if (key == "external_texture")
                capability = EWindowEffectCapability::ExternalTexture;
            else if (key == "rounded_source")
                capability = EWindowEffectCapability::RoundedSource;
            else {
                error = manifestError(manifest, lineNumber, "unsupported [capabilities] key: " + key);
                return false;
            }

            bool enabled = false;
            if (!parseBoolean(value, enabled, parseError)) {
                error = manifestError(manifest, lineNumber, parseError);
                return false;
            }

            const auto bit = windowEffectCapabilityBit(capability);
            if (enabled)
                parsed.spec.capabilities |= bit;
            else
                parsed.spec.capabilities &= ~bit;
            continue;
        }

        error = manifestError(manifest, lineNumber, "key appears outside [effect] or [capabilities]");
        return false;
    }

    if (!parsed.hasName || !parsed.hasShader ||
        !parsed.hasOpenDuration || !parsed.hasCloseDuration) {
        error = manifestError(
            manifest,
            0U,
            "[effect] requires id, shader, open_duration, and close_duration"
        );
        return false;
    }

    if (parsed.spec.displayName.empty())
        parsed.spec.displayName = parsed.spec.name;

    const auto shaderPath = effectRoot / parsed.spec.fragmentShaderAsset;
    std::error_code shaderError;
    if (!std::filesystem::is_regular_file(shaderPath, shaderError) || shaderError) {
        error = manifestError(
            manifest,
            0U,
            "shader file does not exist: " + shaderPath.string()
        );
        return false;
    }

    if (!windowEffectSupports(parsed.spec, EWindowEffectCapability::SourceTexture) ||
        !windowEffectSupports(parsed.spec, EWindowEffectCapability::Texture2D)) {
        error = manifestError(
            manifest,
            0U,
            "source_texture and texture_2d capabilities are required by the current renderer"
        );
        return false;
    }

    output = std::move(parsed.spec);
    return true;
}

} // namespace

SWindowEffectRegistryLoadResult loadWindowEffectRegistry(
    const std::filesystem::path& effectRoot
) {
    SWindowEffectRegistryLoadResult result;

    std::error_code rootError;
    if (!std::filesystem::is_directory(effectRoot, rootError) || rootError) {
        result.error = "effect root is unavailable: " + effectRoot.string();
        return result;
    }

    std::vector<std::filesystem::path> manifests;
    for (std::filesystem::directory_iterator iterator(effectRoot, rootError), end;
         !rootError && iterator != end;
         iterator.increment(rootError)) {
        std::error_code typeError;
        if (!iterator->is_directory(typeError) || typeError)
            continue;

        const auto manifest = iterator->path() / "effect.toml";
        std::error_code manifestErrorCode;
        if (std::filesystem::is_regular_file(manifest, manifestErrorCode) &&
            !manifestErrorCode) {
            manifests.push_back(manifest);
        }
    }

    if (rootError) {
        result.error = "could not scan effect root " + effectRoot.string() + ": " + rootError.message();
        return result;
    }

    std::sort(manifests.begin(), manifests.end());
    if (manifests.empty()) {
        result.error = "no effect.toml manifests found under " + effectRoot.string();
        return result;
    }

    std::vector<SWindowEffectSpec> loaded{{
        .name = std::string{kNoWindowEffect},
        .displayName = "None",
        .fragmentShaderAsset = {},
        .openDurationSeconds = 0.0F,
        .closeDurationSeconds = 0.0F,
        .reversible = true,
        .capabilities = 0U,
    }};
    std::unordered_set<std::string> names{std::string{kNoWindowEffect}};

    for (const auto& manifest : manifests) {
        SWindowEffectSpec effect;
        if (!parseManifest(effectRoot, manifest, effect, result.error))
            return result;

        if (!names.insert(effect.name).second) {
            result.error = manifest.string() + ": duplicate effect id: " + effect.name;
            return result;
        }
        loaded.push_back(std::move(effect));
    }

    g_windowEffectSpecs = std::move(loaded);
    result.success = true;
    result.loadedEffects = g_windowEffectSpecs.size() - 1U;
    return result;
}

std::span<const SWindowEffectSpec> windowEffectSpecs() noexcept {
    return g_windowEffectSpecs;
}

const SWindowEffectSpec* findWindowEffect(std::string_view name) noexcept {
    for (const auto& effect : g_windowEffectSpecs) {
        if (effect.name == name)
            return &effect;
    }
    return nullptr;
}

bool windowEffectIsNone(const SWindowEffectSpec& effect) noexcept {
    return effect.name == kNoWindowEffect;
}

bool windowEffectSupports(
    const SWindowEffectSpec& effect,
    EWindowEffectCapability capability
) noexcept {
    return (effect.capabilities & windowEffectCapabilityBit(capability)) != 0U;
}
