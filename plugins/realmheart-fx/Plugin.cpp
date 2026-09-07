// SPDX-License-Identifier: GPL-3.0-or-later
// Upstream provenance for the target-only compositor lifecycle lives in
// ATTRIBUTION.md.

#define WLR_USE_UNSTABLE

#include "RealmheartEffectPassElement.hpp"
#include "WindowEffectConfig.hpp"
#include "WindowEffectPolicy.hpp"
#include "WindowEffectRegistry.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/core/Subcompositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/version.h>
#include <hyprland/src/xwayland/XSurface.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace Render::GL;

namespace {

constexpr float kSourceWaitTimeoutSeconds = 2.00F;
constexpr float kSlowToolkitSourceWaitTimeoutSeconds = 8.00F;
constexpr float kPassWaitTimeoutSeconds = 0.60F;
constexpr std::uint32_t kOpeningStableSourceFrames = 3;
constexpr std::uint32_t kPlasmaOpeningStableSourceFrames = 1;
constexpr double kPlasmaMinimumOpeningTextureDimension = 32.0;
constexpr float kOpeningGhostAlpha = 1.0F / 255.0F;
constexpr float kMultiSurfaceOpeningSettleSeconds = 0.14F;
constexpr double kNearFullSurfaceAreaRatio = 0.85;
constexpr std::string_view kPlasmaSystemMonitorClass =
    "org.kde.plasma-systemmonitor";
constexpr float kAutomaticOpenDurationScale = 0.82F;
constexpr float kAutomaticCloseDurationScale = 0.82F;
constexpr const char* kCommandName = "realmheart-fx";
constexpr std::size_t kMaximumShaderBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaximumDiagnosticBytes = 256U * 1024U;
constexpr std::size_t kMaximumDiagnosticMessageBytes = 4096U;

constexpr const char* kVertexShader = R"GLSL(#version 300 es
precision highp float;
uniform mat3 proj;
in vec2 pos;
out vec2 v_texcoord;

void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    v_texcoord = pos;
}
)GLSL";

constexpr const char* kSceneBlitFragmentShader = R"GLSL(#version 300 es
precision highp float;
in vec2 v_texcoord;
uniform sampler2D tex;
uniform vec2 resolution;
uniform float radius;
uniform float opacity;
layout(location = 0) out vec4 fragColor;

float roundedBoxSDF(vec2 center, vec2 halfSize, float r) {
    vec2 d = abs(center) - halfSize + r;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;
}

void main() {
    float shapeMask = 1.0;
    if (radius > 0.0) {
        vec2 pixelPos = v_texcoord * resolution;
        float sd = roundedBoxSDF(
            pixelPos - resolution * 0.5,
            resolution * 0.5,
            radius
        );
        shapeMask = 1.0 - smoothstep(-1.0, 1.0, sd);
    }

    fragColor = texture(tex, v_texcoord) *
        shapeMask * clamp(opacity, 0.0, 1.0);
}
)GLSL";

struct SCompiledWindowEffect {
    const SWindowEffectSpec* spec = nullptr;
    std::array<SRealmheartEffectShader, 2> shaders{};

    void destroy() {
        for (auto& shader : shaders)
            shader.destroy();
    }
};

bool sameOverrideValue(
    const Desktop::Types::SAlphaValue& lhs,
    const Desktop::Types::SAlphaValue& rhs
) noexcept {
    return lhs.alpha == rhs.alpha && lhs.overridden == rhs.overridden;
}

template <typename T>
bool sameOverrideValue(const T& lhs, const T& rhs) noexcept {
    return lhs == rhs;
}

template <typename T>
struct SOwnedSetProp {
    using SOverride = Desktop::Types::COverridableVar<T>;

    std::optional<T> previous;
    std::optional<T> owned;

    void capture(const SOverride& value) {
        const auto priority = Desktop::Types::PRIORITY_SET_PROP;
        if (value.hasValue() && value.getPriority() == priority)
            previous = value.value();
        else
            previous.reset();
    }

    void claim(SOverride& value, const T& replacement) {
        owned = replacement;
        value.set(replacement, Desktop::Types::PRIORITY_SET_PROP);
    }

    void refreshIfOwned(SOverride& value, const T& replacement) {
        if (!owned.has_value() || !value.hasValue())
            return;

        const auto priority = Desktop::Types::PRIORITY_SET_PROP;
        if (value.getPriority() == priority &&
            sameOverrideValue(value.value(), *owned)) {
            claim(value, replacement);
        }
    }

    void restore(SOverride& value) {
        if (!owned.has_value())
            return;

        const auto priority = Desktop::Types::PRIORITY_SET_PROP;
        const bool stillOwned =
            value.hasValue() &&
            value.getPriority() == priority &&
            sameOverrideValue(value.value(), *owned);
        if (!stillOwned) {
            previous.reset();
            owned.reset();
            return;
        }

        if (previous.has_value())
            value.set(*previous, priority);
        else
            value.unset(priority);

        previous.reset();
        owned.reset();
    }
};

struct SWindowHiddenState {
    SOwnedSetProp<Desktop::Types::SAlphaValue> alpha;
    SOwnedSetProp<Desktop::Types::SAlphaValue> alphaInactive;
    SOwnedSetProp<Desktop::Types::SAlphaValue> alphaFullscreen;
    SOwnedSetProp<bool> noAnim;
    bool captured = false;

    void capture(Desktop::Rule::CWindowRuleApplicator& applicator) {
        alpha.capture(applicator.alpha());
        alphaInactive.capture(applicator.alphaInactive());
        alphaFullscreen.capture(applicator.alphaFullscreen());
        noAnim.capture(applicator.noAnim());
        captured = true;
    }

    void restore(Desktop::Rule::CWindowRuleApplicator& applicator) {
        if (!captured)
            return;

        alpha.restore(applicator.alpha());
        alphaInactive.restore(applicator.alphaInactive());
        alphaFullscreen.restore(applicator.alphaFullscreen());
        noAnim.restore(applicator.noAnim());
        captured = false;
    }
};

enum class EWindowAnimationMode {
    ManualCycle,
    AutomaticOpen,
    AutomaticClose,
};

struct SRetainedCloseFrame {
    GLuint texture = 0;
    GLuint framebuffer = 0;

    SRetainedCloseFrame() = default;
    SRetainedCloseFrame(const SRetainedCloseFrame&) = delete;
    SRetainedCloseFrame& operator=(const SRetainedCloseFrame&) = delete;

    SRetainedCloseFrame(SRetainedCloseFrame&& other) noexcept
        : texture(std::exchange(other.texture, 0)),
          framebuffer(std::exchange(other.framebuffer, 0)) {}

    SRetainedCloseFrame& operator=(SRetainedCloseFrame&& other) noexcept {
        if (this == &other)
            return *this;
        destroy();
        texture = std::exchange(other.texture, 0);
        framebuffer = std::exchange(other.framebuffer, 0);
        return *this;
    }

    ~SRetainedCloseFrame() {
        destroy();
    }

    void destroy() noexcept {
        if ((framebuffer != 0 || texture != 0) && g_pHyprOpenGL)
            g_pHyprOpenGL->makeEGLCurrent();
        if (framebuffer != 0) {
            glDeleteFramebuffers(1, &framebuffer);
            framebuffer = 0;
        }
        if (texture != 0) {
            glDeleteTextures(1, &texture);
            texture = 0;
        }
    }
};

struct SHeldReflowWindow {
    PHLWINDOWREF window;
    Vector2D oldPosition{};
    Vector2D oldSize{};
    Vector2D finalPosition{};
    Vector2D finalSize{};
    float rounding = 0.0F;
    SRetainedCloseFrame frame{};
    SWindowHiddenState hiddenState{};
    bool applied = false;
    bool hidden = false;
};

struct SWindowAnimation {
    PHLWINDOWREF window;
    PHLMONITORREF monitor;
    const SWindowEffectSpec* effect = nullptr;
    WindowEffectPool candidateEffects;
    EWindowAnimationMode mode = EWindowAnimationMode::ManualCycle;

    // Opening/manual effects sample the live target every frame. Closing owns
    // an immutable copy of the disappearing surface. Tiled closes also retain
    // each survivor's pre-configure pixels, hide the already-resized live
    // clients, and replay those frozen frames until native reflow resumes.
    SRetainedCloseFrame closeFrame{};
    std::vector<SHeldReflowWindow> heldReflowWindows{};
    SWindowHiddenState hiddenState{};
    bool reflowHoldApplied = false;

    CBox box{};
    float rounding = 0.0F;
    std::string windowClass;
    std::chrono::steady_clock::time_point armedTime{};
    std::chrono::steady_clock::time_point startTime{};
    GLuint pendingSourceTexture = 0;
    GLenum pendingSourceTarget = 0;
    CBox pendingSourceBox{};
    std::uint32_t pendingSourceFrames = 0;
    std::uint32_t pendingSourceDepth = 0;
    std::size_t pendingCandidateCount = 0;
    std::uint32_t sourceSurfaceDepth = 0;
    std::size_t sourceCandidateCount = 0;
    GLuint observedSourceTexture = 0;
    GLenum observedSourceTarget = 0;
    Vector2D observedSourceSize{};
    double observedAspectMismatch = std::numeric_limits<double>::infinity();
    bool observedPendingSizeAck = false;
    std::size_t observedPendingSizeAckCount = 0;
    bool active = false;
    bool started = false;
    bool sawPass = false;
    bool terminalFrameQueued = false;
};

struct SPluginState {
    std::vector<SCompiledWindowEffect> effects;
    SRealmheartEffectShader sceneBlitShader;
    SWindowAnimation animation;
    SWindowEffectConfig effectConfig = builtInWindowEffectConfig();
    std::mt19937_64 effectRandom{
        static_cast<std::mt19937_64::result_type>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        ) ^ static_cast<std::mt19937_64::result_type>(
            std::chrono::system_clock::now().time_since_epoch().count()
        )
    };
    std::filesystem::path effectConfigPath;
    std::string effectConfigStatus = "not loaded";
    bool automaticOpenEnabled = true;
    bool automaticCloseEnabled = true;
    wl_event_source* tick = nullptr;
    SP<SHyprCtlCommand> command;
};

HANDLE g_handle = nullptr;
UP<SPluginState> g_state;
std::vector<Hyprutils::Signal::CHyprSignalListener> g_listeners;

float animationDurationSeconds(
    const SWindowAnimation& animation,
    const SWindowEffectSpec& effect
) noexcept {
    if (animation.mode == EWindowAnimationMode::AutomaticOpen)
        return effect.openDurationSeconds * kAutomaticOpenDurationScale;
    if (animation.mode == EWindowAnimationMode::AutomaticClose)
        return effect.closeDurationSeconds * kAutomaticCloseDurationScale;

    return effect.closeDurationSeconds + effect.openDurationSeconds;
}

std::string_view animationModeName(EWindowAnimationMode mode) noexcept {
    switch (mode) {
        case EWindowAnimationMode::ManualCycle:
            return "manual";
        case EWindowAnimationMode::AutomaticOpen:
            return "open";
        case EWindowAnimationMode::AutomaticClose:
            return "close";
    }

    return "unknown";
}

std::filesystem::path diagnosticLogPath() {
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        runtime != nullptr && *runtime != '\0') {
        const std::filesystem::path runtimePath{runtime};
        if (runtimePath.is_absolute())
            return runtimePath / "realmheart-fx.log";
    }

    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        return std::filesystem::path{home} / ".cache" / "realmheart" /
            "realmheart-fx.log";

    return {};
}

std::string boundedDiagnosticMessage(std::string_view message) {
    std::string sanitized;
    sanitized.reserve(std::min(message.size(), kMaximumDiagnosticMessageBytes));
    for (const unsigned char character : message) {
        if (sanitized.size() >= kMaximumDiagnosticMessageBytes)
            break;
        if (character == '\n' || character == '\r' || character == '\t' ||
            character >= 0x20U) {
            sanitized.push_back(static_cast<char>(character));
        } else {
            sanitized.push_back(' ');
        }
    }
    return sanitized;
}

void appendDiagnostic(const std::string& message) noexcept {
    try {
        const std::filesystem::path path = diagnosticLogPath();
        if (path.empty())
            return;

        const auto parent = path.parent_path();
        std::error_code error;
        if (!std::filesystem::exists(parent, error)) {
            std::filesystem::create_directories(parent, error);
            if (error)
                return;
            ::chmod(parent.c_str(), 0700);
        }
        if (error || !std::filesystem::is_directory(parent, error) || error)
            return;

        const int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW;
        int descriptor = ::open(path.c_str(), flags, 0600);
        if (descriptor < 0)
            return;

        struct stat metadata {};
        if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
            ::close(descriptor);
            return;
        }
        ::fchmod(descriptor, 0600);
        const std::string line = boundedDiagnosticMessage(message) + '\n';
        if (static_cast<std::uintmax_t>(metadata.st_size) + line.size() >
            kMaximumDiagnosticBytes) {
            ::close(descriptor);
            descriptor = ::open(
                path.c_str(),
                O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                0600
            );
            if (descriptor < 0)
                return;
            if (::fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
                ::close(descriptor);
                return;
            }
            ::fchmod(descriptor, 0600);
        }

        const char* data = line.data();
        std::size_t remaining = line.size();
        while (remaining > 0U) {
            const ssize_t written = ::write(descriptor, data, remaining);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            data += written;
            remaining -= static_cast<std::size_t>(written);
        }
        ::close(descriptor);
    } catch (...) {
        // Diagnostics must never alter plugin control flow.
    }
}

std::string reloadWindowEffectConfig(bool startup) {
    if (!g_state)
        return "error: plugin state is unavailable";

    const auto requestedPath = defaultWindowEffectConfigPath();
    g_state->effectConfigPath = requestedPath;

    if (requestedPath.empty()) {
        if (startup)
            g_state->effectConfig = builtInWindowEffectConfig();
        g_state->effectConfigStatus =
            "built-in fallback: HOME and XDG_CONFIG_HOME are unavailable";
        appendDiagnostic("window effect config: " + g_state->effectConfigStatus);
        return startup ? "ok: " + g_state->effectConfigStatus
                       : "error: " + g_state->effectConfigStatus;
    }

    std::error_code existsError;
    const bool exists = std::filesystem::exists(requestedPath, existsError);
    if (existsError) {
        const std::string message =
            "could not inspect " + requestedPath.string() + ": " +
            existsError.message();
        if (startup)
            g_state->effectConfig = builtInWindowEffectConfig();
        g_state->effectConfigStatus =
            startup ? "built-in fallback: " + message : message;
        appendDiagnostic("window effect config: " + g_state->effectConfigStatus);
        return "error: " + message;
    }

    if (!exists) {
        g_state->effectConfig = builtInWindowEffectConfig();
        g_state->effectConfig.sourcePath = requestedPath;
        g_state->effectConfigStatus =
            "built-in fallback: config file does not exist";
        const std::string summary = windowEffectConfigSummary(
            g_state->effectConfig,
            requestedPath
        );
        appendDiagnostic("window effect config: " + summary +
                         " note=config file does not exist");
        return "ok: " + summary;
    }

    auto loaded = loadWindowEffectConfig(requestedPath);
    if (!loaded.success) {
        if (startup) {
            g_state->effectConfig = builtInWindowEffectConfig();
            g_state->effectConfig.sourcePath = requestedPath;
        }
        g_state->effectConfigStatus =
            (startup ? "built-in fallback after parse error: "
                     : "reload rejected; previous config preserved: ") +
            loaded.error;
        appendDiagnostic("window effect config: " + g_state->effectConfigStatus);
        return "error: " + loaded.error;
    }

    g_state->effectConfig = std::move(loaded.config);
    g_state->effectConfigStatus = "loaded";
    const std::string summary = windowEffectConfigSummary(
        g_state->effectConfig,
        requestedPath
    );
    appendDiagnostic("window effect config: " + summary);
    return "ok: " + summary;
}

std::string currentWindowEffectsSummary() {
    std::ostringstream output;
    bool first = true;
    for (const auto& effect : windowEffectSpecs()) {
        if (!first)
            output << ',';
        output << effect.name;
        first = false;
    }
    return output.str();
}

std::string currentWindowEffectConfigStatus() {
    if (!g_state)
        return "unavailable";

    return windowEffectConfigSummary(
        g_state->effectConfig,
        g_state->effectConfigPath
    ) + " status=" + g_state->effectConfigStatus;
}

struct SShaderHandleGuard {
    GLuint value = 0;

    explicit SShaderHandleGuard(GLuint handle) noexcept : value(handle) {}

    ~SShaderHandleGuard() {
        if (value != 0)
            glDeleteShader(value);
    }

    SShaderHandleGuard(const SShaderHandleGuard&) = delete;
    SShaderHandleGuard& operator=(const SShaderHandleGuard&) = delete;

    GLuint release() noexcept {
        return std::exchange(value, 0U);
    }
};

struct SProgramHandleGuard {
    GLuint value = 0;

    explicit SProgramHandleGuard(GLuint handle) noexcept : value(handle) {}

    ~SProgramHandleGuard() {
        if (value != 0)
            glDeleteProgram(value);
    }

    SProgramHandleGuard(const SProgramHandleGuard&) = delete;
    SProgramHandleGuard& operator=(const SProgramHandleGuard&) = delete;

    GLuint release() noexcept {
        return std::exchange(value, 0U);
    }
};

GLuint compileShader(GLenum type, const std::string& source) {
    const GLuint shader = glCreateShader(type);
    const char* sourcePointer = source.c_str();
    glShaderSource(shader, 1, &sourcePointer, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
        return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    glDeleteShader(shader);
    throw std::runtime_error("Realmheart FX shader compilation failed: " + log);
}

GLuint createProgram(const std::string& vertex, const std::string& fragment) {
    SShaderHandleGuard vertexShader{compileShader(GL_VERTEX_SHADER, vertex)};
    SShaderHandleGuard fragmentShader{compileShader(GL_FRAGMENT_SHADER, fragment)};
    SProgramHandleGuard program{glCreateProgram()};
    if (program.value == 0)
        throw std::runtime_error("Realmheart FX program allocation failed");

    glAttachShader(program.value, vertexShader.value);
    glAttachShader(program.value, fragmentShader.value);
    glLinkProgram(program.value);

    glDetachShader(program.value, vertexShader.value);
    glDetachShader(program.value, fragmentShader.value);

    GLint linked = GL_FALSE;
    glGetProgramiv(program.value, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE)
        return program.release();

    GLint length = 0;
    glGetProgramiv(program.value, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetProgramInfoLog(program.value, length, nullptr, log.data());
    throw std::runtime_error("Realmheart FX program linking failed: " + log);
}

std::string readBoundedShaderFile(const std::filesystem::path& path) {
    const int descriptor = ::open(
        path.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
    );
    if (descriptor < 0)
        throw std::runtime_error("could not open shader: " + path.string());

    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0) {
        const std::string message = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error(
            "could not inspect shader " + path.string() + ": " + message
        );
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
        static_cast<std::uintmax_t>(metadata.st_size) > kMaximumShaderBytes) {
        ::close(descriptor);
        throw std::runtime_error(
            "shader is not a regular file within the size limit: " + path.string()
        );
    }

    std::string output;
    output.reserve(static_cast<std::size_t>(metadata.st_size));
    std::array<char, 8192> buffer{};
    while (true) {
        const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
        if (count == 0)
            break;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            const std::string message = std::strerror(errno);
            ::close(descriptor);
            throw std::runtime_error(
                "could not read shader " + path.string() + ": " + message
            );
        }
        if (output.size() + static_cast<std::size_t>(count) > kMaximumShaderBytes) {
            ::close(descriptor);
            throw std::runtime_error(
                "shader exceeds the size limit: " + path.string()
            );
        }
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(descriptor);
    return output;
}

std::string readEffectShader(const SWindowEffectSpec& effect) {
    const std::filesystem::path shaderPath =
        defaultWindowEffectAssetRoot() / effect.fragmentShaderAsset;
    try {
        return readBoundedShaderFile(shaderPath);
    } catch (const std::exception& exception) {
        throw std::runtime_error(
            "could not read " + std::string(effect.displayName) +
            " shader at " + shaderPath.string() + ": " + exception.what()
        );
    }
}

bool hasExternalTextureExtension() noexcept {
    const auto* rawExtensions = glGetString(GL_EXTENSIONS);
    if (rawExtensions == nullptr)
        return false;

    std::istringstream extensions{
        reinterpret_cast<const char*>(rawExtensions)
    };
    std::string extension;
    while (extensions >> extension) {
        if (extension == "GL_OES_EGL_image_external_essl3")
            return true;
    }
    return false;
}

std::string shaderWithoutComments(std::string_view source) {
    std::string output{source};
    bool lineComment = false;
    bool blockComment = false;
    for (std::size_t index = 0; index < output.size(); ++index) {
        if (lineComment) {
            if (output[index] == '\n')
                lineComment = false;
            else if (output[index] != '\r')
                output[index] = ' ';
            continue;
        }
        if (blockComment) {
            if (output[index] == '*' && index + 1U < output.size() &&
                output[index + 1U] == '/') {
                output[index] = ' ';
                output[index + 1U] = ' ';
                ++index;
                blockComment = false;
            } else if (output[index] != '\n' && output[index] != '\r') {
                output[index] = ' ';
            }
            continue;
        }
        if (output[index] == '/' && index + 1U < output.size()) {
            if (output[index + 1U] == '/') {
                output[index] = ' ';
                output[index + 1U] = ' ';
                ++index;
                lineComment = true;
            } else if (output[index + 1U] == '*') {
                output[index] = ' ';
                output[index + 1U] = ' ';
                ++index;
                blockComment = true;
            }
        }
    }
    return output;
}

std::string externalVariant(std::string fragment) {
    const std::string uncommented = shaderWithoutComments(fragment);
    const std::regex versionPattern{
        R"((^|\n)[ \t]*#version[ \t]+300[ \t]+es[ \t]*(\r?\n|$))"
    };
    std::smatch versionMatch;
    if (!std::regex_search(uncommented, versionMatch, versionPattern)) {
        throw std::runtime_error(
            "external shader variant requires a #version 300 es directive"
        );
    }
    fragment.insert(
        static_cast<std::size_t>(versionMatch.position() + versionMatch.length()),
        "#extension GL_OES_EGL_image_external_essl3 : require\n"
    );

    const std::regex samplerPattern{
        R"(\buniform[ \t]+(?:(?:lowp|mediump|highp)[ \t]+)?sampler2D[ \t]+tex[ \t]*;)"
    };
    std::sregex_iterator begin{uncommented.begin(), uncommented.end(), samplerPattern};
    const std::sregex_iterator end{};
    if (begin == end || std::next(begin) != end) {
        throw std::runtime_error(
            "external shader variant requires exactly one uniform sampler2D tex"
        );
    }
    const auto samplerMatch = *begin;
    fragment.replace(
        static_cast<std::size_t>(samplerMatch.position()),
        static_cast<std::size_t>(samplerMatch.length()),
        "uniform samplerExternalOES tex;"
    );
    return fragment;
}

void fillLocations(SRealmheartEffectShader& shader) {
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Projection)) =
        glGetUniformLocation(shader.program, "proj");
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Position)) =
        glGetAttribLocation(shader.program, "pos");
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Progress)) =
        glGetUniformLocation(shader.program, "progress");
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Resolution)) =
        glGetUniformLocation(shader.program, "resolution");
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Texture)) =
        glGetUniformLocation(shader.program, "tex");
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Radius)) =
        glGetUniformLocation(shader.program, "radius");
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Reverse)) =
        glGetUniformLocation(shader.program, "reverse");
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Gold)) =
        glGetUniformLocation(shader.program, "uGold");
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Starlight)) =
        glGetUniformLocation(shader.program, "uStarlight");
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Astral)) =
        glGetUniformLocation(shader.program, "uAstral");
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Void)) =
        glGetUniformLocation(shader.program, "uVoid");
    shader.locations.at(static_cast<std::size_t>(ERealmheartEffectUniform::Opacity)) =
        glGetUniformLocation(shader.program, "opacity");
}

SCompiledWindowEffect* compiledEffect(const SWindowEffectSpec* spec) {
    if (!g_state)
        return nullptr;

    for (auto& effect : g_state->effects) {
        if (effect.spec == spec)
            return &effect;
    }
    return nullptr;
}

void initialiseEffects() {
    g_pHyprOpenGL->makeEGLCurrent();
    const bool externalTextureSupported = hasExternalTextureExtension();

    for (const auto& spec : windowEffectSpecs()) {
        if (windowEffectIsNone(spec) || spec.fragmentShaderAsset.empty())
            continue;

        SCompiledWindowEffect compiled{
            .spec = &spec,
        };
        try {
            const std::string fragment = readEffectShader(spec);
            compiled.shaders[0].program = createProgram(kVertexShader, fragment);
            fillLocations(compiled.shaders[0]);

            if (windowEffectSupports(spec, EWindowEffectCapability::ExternalTexture)) {
                if (!externalTextureSupported) {
                    appendDiagnostic(
                        "external texture variant disabled: extension unavailable effect=" +
                        spec.name
                    );
                } else {
                    try {
                        compiled.shaders[1].program = createProgram(
                            kVertexShader,
                            externalVariant(fragment)
                        );
                        fillLocations(compiled.shaders[1]);
                    } catch (const std::exception& exception) {
                        appendDiagnostic(
                            "external texture variant disabled: effect=" + spec.name +
                            " reason=" + exception.what()
                        );
                    }
                }
            }

            g_state->effects.push_back(std::move(compiled));
        } catch (const std::exception& exception) {
            compiled.destroy();
            appendDiagnostic(
                "effect skipped during shader initialization: effect=" +
                spec.name + " reason=" + exception.what()
            );
        }
    }

    g_state->sceneBlitShader.program = createProgram(
        kVertexShader,
        kSceneBlitFragmentShader
    );
    fillLocations(g_state->sceneBlitShader);
}

SP<Render::ITexture> stateTexture(const SP<CWLSurfaceResource>& surface) {
    if (!surface)
        return nullptr;
    if (surface->m_current.texture)
        return surface->m_current.texture;
    if (surface->m_current.buffer && surface->m_current.buffer->m_texture)
        return surface->m_current.buffer->m_texture;
    return nullptr;
}

struct SSurfaceTextureCandidate {
    SP<Render::ITexture> texture;
    std::uint32_t depth = 0;
    double area = 0.0;
    double aspectMismatch = std::numeric_limits<double>::infinity();
};

void collectSurfaceTextureCandidates(
    const SP<CWLSurfaceResource>& surface,
    std::uint32_t depth,
    std::vector<SSurfaceTextureCandidate>& destination
) {
    if (!surface)
        return;

    if (const auto texture = stateTexture(surface);
        texture && texture->m_texID != 0 && texture->m_size.x > 0.0 &&
        texture->m_size.y > 0.0) {
        destination.push_back({
            .texture = texture,
            .depth = depth,
            .area = texture->m_size.x * texture->m_size.y,
        });
    }

    for (auto& weakSubsurface : surface->m_subsurfaces) {
        const auto subsurface = weakSubsurface.lock();
        if (!subsurface || subsurface->m_surface.expired())
            continue;
        collectSurfaceTextureCandidates(
            subsurface->m_surface.lock(),
            depth + 1,
            destination
        );
    }
}

double surfaceTextureAspectMismatch(
    const SP<Render::ITexture>& texture,
    const Vector2D& logicalSize
) noexcept {
    if (!texture || texture->m_size.x <= 0.0 || texture->m_size.y <= 0.0 ||
        logicalSize.x <= 0.0 || logicalSize.y <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }

    const double widthProduct = texture->m_size.x * logicalSize.y;
    const double heightProduct = texture->m_size.y * logicalSize.x;
    const double denominator = std::max(
        std::abs(widthProduct),
        std::abs(heightProduct)
    );
    if (!std::isfinite(widthProduct) || !std::isfinite(heightProduct) ||
        denominator <= 0.0) {
        return std::numeric_limits<double>::infinity();
    }

    return std::abs(widthProduct - heightProduct) / denominator;
}

SP<Render::ITexture> surfaceTexture(
    const PHLWINDOW& window,
    std::uint32_t* selectedDepth = nullptr,
    std::size_t* candidateCount = nullptr
) {
    if (!window)
        return nullptr;

    // Use the same authoritative surface root Hyprland's renderer uses.
    // Protocol-specific pointers can lag during early map for Qt Quick clients,
    // while wlSurface()->resource() already identifies the surface that
    // renderWindow() traverses.
    SP<CWLSurfaceResource> surface;
    if (window->wlSurface())
        surface = window->wlSurface()->resource();

    if (!surface && !window->m_xdgSurface.expired()) {
        const auto xdg = window->m_xdgSurface.lock();
        if (xdg && !xdg->m_surface.expired())
            surface = xdg->m_surface.lock();
    }

    if (!surface && !window->m_xwaylandSurface.expired()) {
        const auto xwayland = window->m_xwaylandSurface.lock();
        if (xwayland && !xwayland->m_surface.expired())
            surface = xwayland->m_surface.lock();
    }

    if (!surface)
        return nullptr;

    std::vector<SSurfaceTextureCandidate> candidates;
    collectSurfaceTextureCandidates(surface, 0, candidates);
    if (candidateCount)
        *candidateCount = candidates.size();
    if (candidates.empty())
        return nullptr;

    const Vector2D logicalSize = window->size(
        Desktop::View::IGeometric::GEOMETRIC_CURRENT
    );
    for (auto& candidate : candidates)
        candidate.aspectMismatch = surfaceTextureAspectMismatch(
            candidate.texture,
            logicalSize
        );

    constexpr double kMaximumAspectMismatch = 0.006;
    const bool hasCoherentCandidate = std::ranges::any_of(
        candidates,
        [](const SSurfaceTextureCandidate& candidate) {
            return candidate.aspectMismatch <= kMaximumAspectMismatch;
        }
    );

    double largestEligibleArea = 0.0;
    for (const auto& candidate : candidates) {
        if (hasCoherentCandidate &&
            candidate.aspectMismatch > kMaximumAspectMismatch) {
            continue;
        }
        largestEligibleArea = std::max(largestEligibleArea, candidate.area);
    }

    const SSurfaceTextureCandidate* best = nullptr;
    for (const auto& candidate : candidates) {
        if (hasCoherentCandidate &&
            candidate.aspectMismatch > kMaximumAspectMismatch) {
            continue;
        }
        if (largestEligibleArea > 0.0 &&
            candidate.area < largestEligibleArea * kNearFullSurfaceAreaRatio) {
            continue;
        }

        if (!best || candidate.depth > best->depth ||
            (candidate.depth == best->depth &&
             candidate.aspectMismatch < best->aspectMismatch) ||
            (candidate.depth == best->depth &&
             candidate.aspectMismatch == best->aspectMismatch &&
             candidate.area > best->area)) {
            // Firefox/Zen can expose an almost-full-size blank root buffer while
            // the real client content lives on a near-full-size child surface.
            // Ignore small popup/video subsurfaces, but prefer the deepest
            // coherent candidate when it covers most of the window.
            best = &candidate;
        }
    }

    if (best && selectedDepth)
        *selectedDepth = best->depth;
    return best ? best->texture : nullptr;
}

GLenum textureTarget(const SP<Render::ITexture>& texture);
bool windowIsFullscreen(const PHLWINDOW& window);

bool copySurfaceTexture(
    const SP<Render::ITexture>& sourceTexture,
    SRetainedCloseFrame& destination,
    std::string& reason
) {
    if (!sourceTexture || sourceTexture->m_texID == 0) {
        reason = "closing surface has no usable texture";
        return false;
    }
    if (textureTarget(sourceTexture) != GL_TEXTURE_2D) {
        reason = "closing surface is not a 2D texture";
        return false;
    }

    const int width = static_cast<int>(sourceTexture->m_size.x);
    const int height = static_cast<int>(sourceTexture->m_size.y);
    if (width <= 0 || height <= 0) {
        reason = "closing surface has invalid dimensions";
        return false;
    }

    g_pHyprOpenGL->makeEGLCurrent();

    GLint previousReadFramebuffer = 0;
    GLint previousDrawFramebuffer = 0;
    GLint previousActiveTexture = 0;
    GLint previousTexture2D = 0;
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glDisable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture2D);

    GLuint sourceFramebuffer = 0;
    glGenFramebuffers(1, &sourceFramebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebuffer);
    glFramebufferTexture2D(
        GL_READ_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        sourceTexture->m_texID,
        0
    );

    SRetainedCloseFrame retained;
    glGenTextures(1, &retained.texture);
    glBindTexture(GL_TEXTURE_2D, retained.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        width,
        height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        nullptr
    );

    glGenFramebuffers(1, &retained.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, retained.framebuffer);
    glFramebufferTexture2D(
        GL_DRAW_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        retained.texture,
        0
    );

    bool captured =
        glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (captured) {
        while (glGetError() != GL_NO_ERROR) {
        }
        glBlitFramebuffer(
            0,
            0,
            width,
            height,
            0,
            0,
            width,
            height,
            GL_COLOR_BUFFER_BIT,
            GL_NEAREST
        );
        captured = glGetError() == GL_NO_ERROR;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
    if (sourceFramebuffer != 0)
        glDeleteFramebuffers(1, &sourceFramebuffer);
    if (scissorEnabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture2D));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));

    if (!captured) {
        reason = "failed to copy the closing surface texture";
        return false;
    }

    destination = std::move(retained);
    return true;
}

bool captureCloseFrame(
    const PHLWINDOW& window,
    SRetainedCloseFrame& destination,
    std::string& reason,
    std::uint32_t* selectedDepth = nullptr,
    std::size_t* candidateCount = nullptr
) {
    if (!window) {
        reason = "closing window is unavailable";
        return false;
    }

    return copySurfaceTexture(
        surfaceTexture(window, selectedDepth, candidateCount),
        destination,
        reason
    );
}

std::string_view effectiveWindowClass(const PHLWINDOW& window) noexcept;
void setWindowHidden(
    const PHLWINDOW& window,
    SWindowHiddenState& hiddenState,
    float alpha = 0.0F
);
void clearWindowHidden(
    const PHLWINDOW& window,
    SWindowHiddenState& hiddenState
);

bool captureTiledReflowHold(
    const PHLWINDOW& closingWindow,
    const PHLMONITOR& monitor,
    std::vector<SHeldReflowWindow>& destination,
    std::string& reason
) {
    destination.clear();
    if (!closingWindow || !monitor || closingWindow->m_isFloating)
        return true;
    if (!Desktop::windowState()) {
        reason = "Hyprland window state is unavailable";
        return false;
    }

    for (const auto& candidate : Desktop::windowState()->windows()) {
        if (!candidate || candidate == closingWindow || !candidate->m_isMapped ||
            candidate->isHidden() || candidate->m_isFloating ||
            candidate->m_workspace != closingWindow->m_workspace ||
            candidate->m_monitor.lock() != monitor ||
            windowIsFullscreen(candidate)) {
            continue;
        }

        const Vector2D oldPosition = candidate->position(
            Desktop::View::IGeometric::GEOMETRIC_CURRENT
        );
        const Vector2D oldSize = candidate->size(
            Desktop::View::IGeometric::GEOMETRIC_CURRENT
        );
        if (oldSize.x < 5.0 || oldSize.y < 5.0)
            continue;

        SRetainedCloseFrame frame;
        std::string captureReason;
        if (!copySurfaceTexture(surfaceTexture(candidate), frame, captureReason)) {
            reason = "failed to freeze surviving window " +
                std::string(effectiveWindowClass(candidate)) + ": " +
                captureReason;
            destination.clear();
            return false;
        }

        destination.push_back({
            .window = candidate,
            .oldPosition = oldPosition,
            .oldSize = oldSize,
            .rounding = candidate->rounding(),
            .frame = std::move(frame),
        });
    }

    return true;
}

void applyTiledReflowHold(SWindowAnimation& animation) {
    if (animation.mode != EWindowAnimationMode::AutomaticClose ||
        animation.heldReflowWindows.empty()) {
        return;
    }

    std::size_t applied = 0;
    for (auto& hold : animation.heldReflowWindows) {
        const auto window = hold.window.lock();
        if (!window || !window->m_isMapped || window->isHidden() ||
            window->m_isFloating || windowIsFullscreen(window)) {
            continue;
        }

        auto& position = window->positionAnimation();
        auto& size = window->sizeAnimation();
        if (!position || !size)
            continue;

        if (!hold.applied) {
            hold.finalPosition = window->position(
                Desktop::View::IGeometric::GEOMETRIC_GOAL
            );
            hold.finalSize = window->size(
                Desktop::View::IGeometric::GEOMETRIC_GOAL
            );
            hold.applied = hold.finalSize.x >= 5.0 && hold.finalSize.y >= 5.0;
        }
        if (!hold.applied)
            continue;

        position->setValueAndWarp(hold.oldPosition);
        size->setValueAndWarp(hold.oldSize);

        if (!hold.hidden) {
            setWindowHidden(window, hold.hiddenState);
            hold.hidden = true;
        }

        g_pHyprRenderer->damageBox(CBox{hold.oldPosition, hold.oldSize});
        ++applied;
    }

    if (!animation.reflowHoldApplied) {
        animation.reflowHoldApplied = true;
        appendDiagnostic(
            "close tiled survivor scene frozen: windows=" +
            std::to_string(applied)
        );
    }
}

void releaseTiledReflowHold(SWindowAnimation& animation) {
    if (animation.heldReflowWindows.empty())
        return;

    std::size_t released = 0;
    for (auto& hold : animation.heldReflowWindows) {
        const auto window = hold.window.lock();
        if (!window || !window->m_isMapped || window->isHidden() ||
            window->m_isFloating || windowIsFullscreen(window)) {
            continue;
        }

        auto& position = window->positionAnimation();
        auto& size = window->sizeAnimation();

        if (hold.hidden) {
            clearWindowHidden(window, hold.hiddenState);
            hold.hidden = false;
        }

        if (hold.applied && position && size) {
            position->setValueAndWarp(hold.oldPosition);
            size->setValueAndWarp(hold.oldSize);
            *position = hold.finalPosition;
            *size = hold.finalSize;

            g_pHyprRenderer->damageBox(CBox{hold.oldPosition, hold.oldSize});
            g_pHyprRenderer->damageBox(CBox{hold.finalPosition, hold.finalSize});
        }
        ++released;
    }

    appendDiagnostic(
        "close tiled survivor scene released: windows=" +
        std::to_string(released)
    );
}


std::string_view effectiveWindowClass(const PHLWINDOW& window) noexcept {
    if (!window)
        return {};
    if (!window->m_class.empty())
        return window->m_class;
    return window->m_initialClass;
}

GLenum textureTarget(const SP<Render::ITexture>& texture) {
    if (!texture)
        return 0;

    switch (texture->m_type) {
        case Render::TEXTURE_RGBA:
        case Render::TEXTURE_RGBX:
            return GL_TEXTURE_2D;
        case Render::TEXTURE_EXTERNAL:
            return GL_TEXTURE_EXTERNAL_OES;
        default:
            return 0;
    }
}

bool effectSupportsTarget(
    const SWindowEffectSpec& effect,
    GLenum target,
    bool roundedSource
) noexcept {
    const bool textureSupported =
        (target == GL_TEXTURE_2D &&
            windowEffectSupports(effect, EWindowEffectCapability::Texture2D)) ||
        (target == GL_TEXTURE_EXTERNAL_OES &&
         windowEffectSupports(effect, EWindowEffectCapability::ExternalTexture));
    return textureSupported &&
        (!roundedSource ||
         windowEffectSupports(effect, EWindowEffectCapability::RoundedSource));
}

WindowEffectCapabilityMask requiredCapabilitiesForTarget(
    GLenum target,
    bool roundedSource
) noexcept {
    WindowEffectCapabilityMask required = 0U;
    if (target == GL_TEXTURE_2D)
        required |= windowEffectCapabilityBit(EWindowEffectCapability::Texture2D);
    else if (target == GL_TEXTURE_EXTERNAL_OES)
        required |= windowEffectCapabilityBit(EWindowEffectCapability::ExternalTexture);
    else
        return 0U;
    if (roundedSource)
        required |= windowEffectCapabilityBit(EWindowEffectCapability::RoundedSource);
    return required;
}

bool windowIsFullscreen(const PHLWINDOW& window) {
    return window && Fullscreen::controller() &&
        Fullscreen::controller()->isFullscreen(window);
}

CBox currentWindowRenderBox(const PHLWINDOW& window) {
    if (!window)
        return {};

    Vector2D position = window->position(
        Desktop::View::IGeometric::GEOMETRIC_CURRENT
    );
    const Vector2D size = window->size(
        Desktop::View::IGeometric::GEOMETRIC_CURRENT
    );

    // Match Hyprland's live renderWindow geometry. The effect follows this box
    // every frame, so a late client configure cannot create a snapshot/live
    // handoff resize.
    if (window->m_workspace && !window->m_pinned)
        position += window->m_workspace->m_renderOffset->value();
    position += window->m_floatingOffset;

    return CBox{
        position.x,
        position.y,
        std::max(size.x, 5.0),
        std::max(size.y, 5.0),
    };
}

bool validBox(const CBox& box) noexcept {
    return std::isfinite(box.x) && std::isfinite(box.y) &&
        std::isfinite(box.width) && std::isfinite(box.height) &&
        box.width >= 5.0 && box.height >= 5.0;
}

bool boxesEquivalent(const CBox& lhs, const CBox& rhs) noexcept {
    constexpr double tolerance = 0.5;
    return std::abs(lhs.x - rhs.x) <= tolerance &&
        std::abs(lhs.y - rhs.y) <= tolerance &&
        std::abs(lhs.width - rhs.width) <= tolerance &&
        std::abs(lhs.height - rhs.height) <= tolerance;
}

bool textureGeometryCoherent(
    const PHLWINDOW& window,
    const SP<Render::ITexture>& texture
) noexcept {
    if (!window)
        return false;

    constexpr double kMaximumAspectMismatch = 0.006;
    return surfaceTextureAspectMismatch(
        texture,
        window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)
    ) <= kMaximumAspectMismatch;
}

bool openingSourceReady(
    const PHLWINDOW& window,
    const SP<Render::ITexture>& texture,
    GLenum target,
    bool relaxedToolkitReadiness
) noexcept {
    if (!window || !window->m_isMapped || !texture || texture->m_texID == 0 ||
        target == 0 || !validBox(currentWindowRenderBox(window))) {
        return false;
    }

    if (relaxedToolkitReadiness) {
        // Plasma System Monitor can publish its first usable Qt Quick buffer
        // before configure bookkeeping and decorated-window aspect converge.
        // Waiting for those later states only adds a visible blank pause. The
        // authoritative wlSurface texture is sufficient once it is non-trivial;
        // the opening effect samples it live on every following compositor frame.
        return std::isfinite(texture->m_size.x) &&
            std::isfinite(texture->m_size.y) &&
            texture->m_size.x >= kPlasmaMinimumOpeningTextureDimension &&
            texture->m_size.y >= kPlasmaMinimumOpeningTextureDimension;
    }

    // Only reject the known half-configured state. Once playback starts, the
    // source remains live and geometry is read again on every compositor frame.
    return !window->m_pendingSizeAck.has_value() &&
        window->m_pendingSizeAcks.empty() &&
        textureGeometryCoherent(window, texture);
}

void applyAlphaNow(const PHLWINDOW& window) {
    if (!window)
        return;

    window->updateDecorationValues();
    auto& alpha = window->alpha(Desktop::View::WINDOW_ALPHA_ACTIVE);
    alpha->setValueAndWarp(alpha->goal());
}

void setWindowHidden(
    const PHLWINDOW& window,
    SWindowHiddenState& hiddenState,
    float alpha
) {
    if (!window || !window->m_ruleApplicator)
        return;

    const auto hidden = Desktop::Types::SAlphaValue{
        .alpha = std::clamp(alpha, 0.0F, 1.0F),
        .overridden = true,
    };

    if (!hiddenState.captured) {
        hiddenState.capture(*window->m_ruleApplicator);
        hiddenState.alpha.claim(window->m_ruleApplicator->alpha(), hidden);
        hiddenState.alphaInactive.claim(
            window->m_ruleApplicator->alphaInactive(),
            hidden
        );
        hiddenState.alphaFullscreen.claim(
            window->m_ruleApplicator->alphaFullscreen(),
            hidden
        );
        hiddenState.noAnim.claim(window->m_ruleApplicator->noAnim(), true);
        applyAlphaNow(window);
        return;
    }

    // Do not overwrite a newer set-prop owner. This function is normally
    // called once per hidden state, but retaining the guard makes repeated
    // reflow holds safe if another rule writer races the initial claim.
    hiddenState.alpha.refreshIfOwned(
        window->m_ruleApplicator->alpha(),
        hidden
    );
    hiddenState.alphaInactive.refreshIfOwned(
        window->m_ruleApplicator->alphaInactive(),
        hidden
    );
    hiddenState.alphaFullscreen.refreshIfOwned(
        window->m_ruleApplicator->alphaFullscreen(),
        hidden
    );
    hiddenState.noAnim.refreshIfOwned(window->m_ruleApplicator->noAnim(), true);
    applyAlphaNow(window);
}

void clearWindowHidden(
    const PHLWINDOW& window,
    SWindowHiddenState& hiddenState
) {
    if (!window || !window->m_ruleApplicator)
        return;

    hiddenState.restore(*window->m_ruleApplicator);
    applyAlphaNow(window);

    CBox box = currentWindowRenderBox(window);
    box.expand(2);
    g_pHyprRenderer->damageBox(box);
}

void damageExpandedBox(CBox box) {
    if (!validBox(box))
        return;
    box.expand(2);
    g_pHyprRenderer->damageBox(box);
}

void settleWorkspaceReflowForOpening(const PHLWINDOW& target) {
    if (!g_pCompositor || !target || !target->m_workspace)
        return;

    const auto workspace = target->m_workspace;
    if (!Desktop::windowState())
        return;

    for (const auto& candidate : Desktop::windowState()->windows()) {
        if (!candidate || candidate == target || !candidate->m_isMapped ||
            candidate->m_workspace != workspace) {
            continue;
        }

        // Opening another tiled window changes the goals of every sibling in
        // that layout. Leaving those sibling geometry animations running under
        // a transparent post-window effect lets their pixels travel through the
        // new target's box and makes Realmheart appear to animate the wrong
        // window. Settle only the affected workspace before hiding the target.
        candidate->finishAnimation();
        g_pHyprRenderer->damageWindow(candidate, true);
    }
}

void cancelAnimation(const std::string& reason, bool restoreWindow = true) {
    if (!g_state || !g_state->animation.active)
        return;

    const auto mode = g_state->animation.mode;
    const auto window = g_state->animation.window.lock();
    const CBox oldBox = g_state->animation.box;

    if (restoreWindow && mode != EWindowAnimationMode::AutomaticClose)
        clearWindowHidden(window, g_state->animation.hiddenState);
    if (mode == EWindowAnimationMode::AutomaticClose)
        releaseTiledReflowHold(g_state->animation);

    g_pHyprRenderer->m_renderPass.removeAllOfType(
        "CRealmheartEffectPassElement"
    );
    g_state->animation = {};
    damageExpandedBox(oldBox);

    appendDiagnostic(
        "animation ended: mode=" + std::string(animationModeName(mode)) +
        " reason=" + reason
    );
}

std::string armFocusedWindow(std::string_view effectName = "void") {
    const SWindowEffectSpec* effect = findWindowEffect(effectName);
    if (effect == nullptr)
        return "unknown effect: " + std::string(effectName);

    if (windowEffectIsNone(*effect)) {
        cancelAnimation("effect set to none");
        appendDiagnostic("manual test bypassed: effect=none");
        return "ok";
    }

    if (!effect->reversible)
        return "effect does not support the manual close-then-open cycle";
    if (compiledEffect(effect) == nullptr)
        return "effect is registered but its shader is unavailable";

    const PHLWINDOW window = Desktop::focusState()->window();
    if (!window || !window->m_isMapped)
        return "focus a mapped application window first";
    if (windowIsFullscreen(window))
        return "fullscreen windows are excluded";
    if (window->isX11OverrideRedirect())
        return "override-redirect windows are excluded";
    if (!window->m_ruleApplicator)
        return "focused window has no rule applicator";

    if (g_state->animation.active)
        cancelAnimation("manual test restarted");

    window->finishAnimation();
    SWindowHiddenState hiddenState;
    setWindowHidden(window, hiddenState);
    const auto now = std::chrono::steady_clock::now();
    g_state->animation = {
        .window = window,
        .monitor = window->m_monitor.lock(),
        .effect = effect,
        .candidateEffects = WindowEffectPool{std::string{effect->name}},
        .mode = EWindowAnimationMode::ManualCycle,
        .closeFrame = {},
        .hiddenState = std::move(hiddenState),
        .box = currentWindowRenderBox(window),
        .rounding = window->rounding(),
        .windowClass = std::string{effectiveWindowClass(window)},
        .armedTime = now,
        .startTime = {},
        .active = true,
        .started = false,
        .sawPass = false,
        .terminalFrameQueued = false,
    };

    appendDiagnostic(
        "manual animation armed: effect=" + std::string(effect->name)
    );
    damageExpandedBox(g_state->animation.box);
    return "ok";
}

bool automaticOpenEligibility(
    const PHLWINDOW& window,
    const SWindowEffectSpec& effect,
    std::string& reason
) {
    if (!window || !window->m_isMapped) {
        reason = "window is not mapped";
        return false;
    }
    if (!window->m_ruleApplicator) {
        reason = "window has no rule applicator";
        return false;
    }
    if (!window->m_monitor.lock()) {
        reason = "window has no monitor";
        return false;
    }
    if (windowIsFullscreen(window)) {
        reason = "fullscreen window";
        return false;
    }
    if (window->isX11OverrideRedirect()) {
        reason = "override-redirect window";
        return false;
    }
    if (window->parent()) {
        reason = "transient or dialog window";
        return false;
    }
    if (window->m_workspace && !window->m_workspace->isVisible()) {
        reason = "window opened on a non-visible workspace";
        return false;
    }
    if (effect.openDurationSeconds <= 0.0F) {
        reason = "effect has no valid open duration";
        return false;
    }
    if (compiledEffect(&effect) == nullptr) {
        reason = "effect shader is unavailable";
        return false;
    }

    return true;
}

bool automaticCloseEligibility(
    const PHLWINDOW& window,
    const SWindowEffectSpec& effect,
    std::string& reason
) {
    if (!window || !window->m_isMapped) {
        reason = "window is not mapped";
        return false;
    }
    if (!window->m_ruleApplicator) {
        reason = "window has no rule applicator";
        return false;
    }
    if (!window->m_monitor.lock()) {
        reason = "window has no monitor";
        return false;
    }
    if (windowIsFullscreen(window)) {
        reason = "fullscreen window";
        return false;
    }
    if (window->isX11OverrideRedirect()) {
        reason = "override-redirect window";
        return false;
    }
    if (window->parent()) {
        reason = "transient or dialog window";
        return false;
    }
    if (window->m_workspace && !window->m_workspace->isVisible()) {
        reason = "window closed on a non-visible workspace";
        return false;
    }
    if (effect.closeDurationSeconds <= 0.0F) {
        reason = "effect has no valid close duration";
        return false;
    }
    if (compiledEffect(&effect) == nullptr) {
        reason = "effect shader is unavailable";
        return false;
    }

    return true;
}

void onWindowOpen(PHLWINDOW window) {
    if (!g_state || !g_state->automaticOpenEnabled || !window)
        return;

    const std::string_view windowClass = effectiveWindowClass(window);
    const auto& effectPool = automaticOpenEffectsForWindow(
        g_state->effectConfig,
        windowClass,
        window->m_title
    );
    const std::string_view effectName = chooseWindowEffect(
        effectPool,
        g_state->effectRandom()
    );
    if (effectName == kNoWindowEffect) {
        appendDiagnostic(
            "automatic open skipped: reason=assignment resolved to none or class is excluded"
        );
        return;
    }

    const SWindowEffectSpec* effect = findWindowEffect(effectName);
    if (effect == nullptr) {
        appendDiagnostic(
            "automatic open skipped: reason=assigned effect is missing"
        );
        return;
    }

    if (g_state->animation.active) {
        appendDiagnostic(
            "automatic open skipped: reason=another animation is active"
        );
        return;
    }

    std::string reason;
    if (!automaticOpenEligibility(window, *effect, reason)) {
        appendDiagnostic(
            "automatic open skipped: reason=" + reason
        );
        return;
    }

    // Target-only lifecycle: own only this target. Stop Hyprland's already
    // armed geometry transition, settle sibling layout reflow, hide the native
    // target, and sample its live surface plus current geometry on every frame.
    window->finishAnimation();
    settleWorkspaceReflowForOpening(window);
    // Exact zero alpha makes Hyprland cull renderWindow entirely. Some
    // multi-surface clients then do not submit their real content until they are
    // revealed. Keep the target at one 8-bit alpha step: visually imperceptible,
    // but still rendered so frame callbacks and subsurface commits continue.
    SWindowHiddenState hiddenState;
    setWindowHidden(window, hiddenState, kOpeningGhostAlpha);

    const auto now = std::chrono::steady_clock::now();
    g_state->animation = {
        .window = window,
        .monitor = window->m_monitor.lock(),
        .effect = effect,
        .candidateEffects = effectPool,
        .mode = EWindowAnimationMode::AutomaticOpen,
        .closeFrame = {},
        .hiddenState = std::move(hiddenState),
        .box = currentWindowRenderBox(window),
        .rounding = window->rounding(),
        .windowClass = std::string(windowClass),
        .armedTime = now,
        .startTime = {},
        .active = true,
        .started = false,
        .sawPass = false,
        .terminalFrameQueued = false,
    };

    appendDiagnostic(
        "automatic open armed: effect=" + std::string(effect->name) +
        " source=live-target ghostAlpha=" +
        std::to_string(kOpeningGhostAlpha)
    );
    damageExpandedBox(g_state->animation.box);
}

void onRenderStage(eRenderStage stage) {
    if (!g_state || !g_state->animation.active)
        return;

    auto& animation = g_state->animation;
    if (stage == RENDER_PRE_WINDOWS) {
        applyTiledReflowHold(animation);
        return;
    }
    if (stage != RENDER_POST_WINDOWS)
        return;
    const auto currentMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    if (!currentMonitor) {
        cancelAnimation("render monitor disappeared");
        return;
    }
    const auto window = animation.window.lock();

    SP<Render::ITexture> liveTexture;
    GLuint sourceTextureId = 0;
    GLenum target = 0;
    CBox effectBox{};
    float rounding = 0.0F;

    if (animation.mode == EWindowAnimationMode::AutomaticClose) {
        const auto targetMonitor = animation.monitor.lock();
        if (!targetMonitor) {
            cancelAnimation("closing target monitor disappeared");
            return;
        }
        if (targetMonitor != currentMonitor)
            return;

        sourceTextureId = animation.closeFrame.texture;
        target = GL_TEXTURE_2D;
        effectBox = animation.box;
        rounding = animation.rounding;
    } else {
        if (!window || !window->m_isMapped) {
            cancelAnimation("window disappeared");
            return;
        }

        const auto liveMonitor = window->m_monitor.lock();
        if (!liveMonitor) {
            cancelAnimation("window monitor disappeared");
            return;
        }
        if (currentMonitor != liveMonitor)
            return;

        liveTexture = surfaceTexture(
            window,
            &animation.sourceSurfaceDepth,
            &animation.sourceCandidateCount
        );
        target = textureTarget(liveTexture);
        animation.observedSourceTexture =
            liveTexture ? liveTexture->m_texID : 0;
        animation.observedSourceTarget = target;
        animation.observedSourceSize =
            liveTexture ? liveTexture->m_size : Vector2D{};
        animation.observedAspectMismatch = surfaceTextureAspectMismatch(
            liveTexture,
            window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)
        );
        animation.observedPendingSizeAck =
            window->m_pendingSizeAck.has_value();
        animation.observedPendingSizeAckCount =
            window->m_pendingSizeAcks.size();

        const CBox previousBox = animation.box;
        effectBox = currentWindowRenderBox(window);

        if (animation.mode == EWindowAnimationMode::AutomaticOpen &&
            !animation.started) {
            const float armedElapsed = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - animation.armedTime
            ).count();

            // Multi-surface toolkits can publish a placeholder root before the
            // near-full-size child that contains the real application UI. Give
            // that surface tree a short bounded settle window before locking the
            // source. Single-surface clients retain the existing fast path.
            if (animation.sourceCandidateCount > 1 &&
                armedElapsed < kMultiSurfaceOpeningSettleSeconds) {
                animation.pendingSourceTexture = 0;
                animation.pendingSourceTarget = 0;
                animation.pendingSourceBox = {};
                animation.pendingSourceFrames = 0;
                animation.pendingSourceDepth = 0;
                animation.pendingCandidateCount = 0;
                return;
            }

            const bool relaxedToolkitReadiness =
                animation.windowClass == kPlasmaSystemMonitorClass;
            if (!openingSourceReady(
                    window,
                    liveTexture,
                    target,
                    relaxedToolkitReadiness
                )) {
                animation.pendingSourceTexture = 0;
                animation.pendingSourceTarget = 0;
                animation.pendingSourceBox = {};
                animation.pendingSourceFrames = 0;
                animation.pendingSourceDepth = 0;
                animation.pendingCandidateCount = 0;
                return;
            }

            const bool sameCandidate =
                animation.pendingSourceTexture == liveTexture->m_texID &&
                animation.pendingSourceTarget == target &&
                animation.pendingSourceDepth == animation.sourceSurfaceDepth &&
                animation.pendingCandidateCount == animation.sourceCandidateCount &&
                boxesEquivalent(animation.pendingSourceBox, effectBox);
            if (sameCandidate) {
                ++animation.pendingSourceFrames;
            } else {
                animation.pendingSourceTexture = liveTexture->m_texID;
                animation.pendingSourceTarget = target;
                animation.pendingSourceBox = effectBox;
                animation.pendingSourceFrames = 1;
                animation.pendingSourceDepth = animation.sourceSurfaceDepth;
                animation.pendingCandidateCount = animation.sourceCandidateCount;
            }

            const std::uint32_t requiredStableFrames =
                relaxedToolkitReadiness
                ? kPlasmaOpeningStableSourceFrames
                : kOpeningStableSourceFrames;
            if (animation.pendingSourceFrames < requiredStableFrames)
                return;
        }

        if (!liveTexture || liveTexture->m_texID == 0 || target == 0)
            return;
        sourceTextureId = liveTexture->m_texID;

        rounding = window->rounding();
        animation.monitor = liveMonitor;
        animation.box = effectBox;
        animation.rounding = rounding;

        if (!boxesEquivalent(previousBox, effectBox)) {
            damageExpandedBox(previousBox);
            damageExpandedBox(effectBox);
        }
    }

    if (sourceTextureId == 0 || !validBox(effectBox))
        return;
    const bool roundedSource = rounding > 0.0F;
    const auto requiredCapabilities = requiredCapabilitiesForTarget(
        target,
        roundedSource
    );
    if (requiredCapabilities == 0U) {
        cancelAnimation("unsupported source texture target");
        return;
    }
    const SWindowEffectSpec* effect = animation.effect;
    if (effect == nullptr || !effectSupportsTarget(*effect, target, roundedSource)) {
        const std::string_view replacement = chooseWindowEffect(
            animation.candidateEffects,
            g_state->effectRandom(),
            requiredCapabilities
        );
        effect = findWindowEffect(replacement);
        if (effect == nullptr || !effectSupportsTarget(*effect, target, roundedSource)) {
            cancelAnimation("no compatible effect for source target");
            return;
        }
        animation.effect = effect;
    }
    SCompiledWindowEffect* compiled = compiledEffect(effect);
    if (compiled == nullptr) {
        cancelAnimation("selected effect became unavailable");
        return;
    }

    if (!animation.started) {
        animation.started = true;
        animation.startTime = std::chrono::steady_clock::now();
        appendDiagnostic(
            "animation started: mode=" +
            std::string(animationModeName(animation.mode)) +
            " effect=" + std::string(effect->name) +
            " source=" +
            (animation.mode == EWindowAnimationMode::AutomaticClose
                 ? "owned-surface-copy"
                 : "live-target") +
            " box=" + std::to_string(static_cast<int>(effectBox.width)) + "x" +
            std::to_string(static_cast<int>(effectBox.height)) +
            " pos=" + std::to_string(static_cast<int>(effectBox.x)) + "," +
            std::to_string(static_cast<int>(effectBox.y)) +
            " rounding=" + std::to_string(rounding) +
            (animation.mode == EWindowAnimationMode::AutomaticOpen
                 ? " stableFrames=" +
                       std::to_string(animation.pendingSourceFrames)
                 : "") +
            " surfaceDepth=" +
            std::to_string(animation.sourceSurfaceDepth) +
            " surfaceCandidates=" +
            std::to_string(animation.sourceCandidateCount) +
            (animation.mode == EWindowAnimationMode::AutomaticOpen
                 ? " armedToStartMs=" +
                       std::to_string(static_cast<int>(
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               animation.startTime - animation.armedTime
                           ).count()
                       ))
                 : "")
        );
    }

    if (animation.mode == EWindowAnimationMode::ManualCycle &&
        (effect->closeDurationSeconds <= 0.0F ||
         effect->openDurationSeconds <= 0.0F)) {
        cancelAnimation("selected effect has invalid manual-cycle durations");
        return;
    }

    const float duration = animationDurationSeconds(animation, *effect);
    if (duration <= 0.0F) {
        cancelAnimation("selected effect has invalid duration");
        return;
    }

    const float elapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - animation.startTime
    ).count();

    float progress = 0.0F;
    bool reverse = false;
    if (animation.mode == EWindowAnimationMode::AutomaticOpen) {
        progress = std::clamp(elapsed / duration, 0.0F, 1.0F);
        reverse = true;
    } else if (animation.mode == EWindowAnimationMode::AutomaticClose) {
        progress = std::clamp(elapsed / duration, 0.0F, 1.0F);
    } else if (elapsed < effect->closeDurationSeconds) {
        progress = std::clamp(
            elapsed / effect->closeDurationSeconds,
            0.0F,
            1.0F
        );
    } else {
        progress = std::clamp(
            (elapsed - effect->closeDurationSeconds) /
                effect->openDurationSeconds,
            0.0F,
            1.0F
        );
        reverse = true;
    }

    const bool terminalProgress = progress >= 1.0F &&
        (animation.mode != EWindowAnimationMode::ManualCycle || reverse);
    if (terminalProgress)
        animation.terminalFrameQueued = true;

    const std::size_t shaderIndex = target == GL_TEXTURE_EXTERNAL_OES ? 1U : 0U;
    const auto& shader = compiled->shaders[shaderIndex];
    if (shader.program == 0) {
        cancelAnimation("selected effect has no shader for source texture type");
        return;
    }

    if (animation.mode == EWindowAnimationMode::AutomaticClose &&
        !animation.heldReflowWindows.empty()) {
        if (g_state->sceneBlitShader.program == 0) {
            cancelAnimation("frozen-survivor blit shader is unavailable");
            return;
        }

        for (const auto& hold : animation.heldReflowWindows) {
            if (!hold.applied || hold.frame.texture == 0)
                continue;

            g_pHyprRenderer->m_renderPass.add(
                makeUnique<CRealmheartEffectPassElement>(
                    CRealmheartEffectPassElement::SData{
                        .box = CBox{hold.oldPosition, hold.oldSize},
                        .progress = 0.0F,
                        .texture = hold.frame.texture,
                        .textureTarget = GL_TEXTURE_2D,
                        .rounding = hold.rounding,
                        .reverse = false,
                        .opacity = 1.0F,
                        .shader = &g_state->sceneBlitShader,
                    }
                )
            );
        }
    }

    g_pHyprRenderer->m_renderPass.add(
        makeUnique<CRealmheartEffectPassElement>(
            CRealmheartEffectPassElement::SData{
                .box = effectBox,
                .progress = progress,
                .texture = sourceTextureId,
                .textureTarget = target,
                .rounding = rounding,
                .reverse = reverse,
                .opacity = 1.0F,
                .shader = &shader,
            }
        )
    );

    if (!animation.sawPass) {
        animation.sawPass = true;
        appendDiagnostic(
            "visible render pass queued: mode=" +
            std::string(animationModeName(animation.mode)) +
            " effect=" + std::string(effect->name) +
            " texture=" + std::to_string(sourceTextureId) +
            " target=" + std::to_string(target) +
            " box=" + std::to_string(static_cast<int>(effectBox.width)) + "x" +
            std::to_string(static_cast<int>(effectBox.height)) +
            " pos=" + std::to_string(static_cast<int>(effectBox.x)) + "," +
            std::to_string(static_cast<int>(effectBox.y))
        );

        if (animation.mode == EWindowAnimationMode::ManualCycle && g_handle) {
            HyprlandAPI::addNotification(
                g_handle,
                "[Realmheart FX] target-only render pass active",
                CHyprColor{0.35F, 0.76F, 1.0F, 1.0F},
                2500.0F
            );
        }
    }
}

int onTick(void* data) {
    (void)data;

    if (!g_state)
        return 0;

    if (!g_state->animation.active) {
        wl_event_source_timer_update(g_state->tick, 250);
        return 0;
    }

    auto& animation = g_state->animation;
    const SWindowEffectSpec* effect = animation.effect;
    const auto window = animation.window.lock();
    const bool closing = animation.mode == EWindowAnimationMode::AutomaticClose;

    if (effect == nullptr) {
        cancelAnimation("selected effect disappeared during tick");
    } else if (!closing && (!window || !window->m_isMapped)) {
        cancelAnimation("window disappeared during tick");
    } else if (animation.mode == EWindowAnimationMode::AutomaticOpen &&
               automaticWindowClassIsExcluded(effectiveWindowClass(window))) {
        cancelAnimation("window class became excluded during opening");
    } else if (animation.mode == EWindowAnimationMode::AutomaticOpen &&
               windowIsFullscreen(window)) {
        cancelAnimation("window became fullscreen during opening");
    } else if (animation.mode == EWindowAnimationMode::AutomaticOpen &&
               window->m_workspace && !window->m_workspace->isVisible()) {
        cancelAnimation("window moved to a non-visible workspace during opening");
    } else if (closing && !animation.monitor.lock()) {
        cancelAnimation("closing target monitor disappeared");
    } else if (closing &&
               animation.closeFrame.texture == 0) {
        cancelAnimation("retained closing target disappeared");
    } else {
        const auto now = std::chrono::steady_clock::now();
        const float armedElapsed = std::chrono::duration<float>(
            now - animation.armedTime
        ).count();

        if (!animation.started) {
            const float sourceWaitTimeout =
                animation.windowClass == kPlasmaSystemMonitorClass
                ? kSlowToolkitSourceWaitTimeoutSeconds
                : kSourceWaitTimeoutSeconds;
            if (armedElapsed >= sourceWaitTimeout) {
                appendDiagnostic(
                    "opening source timeout: texture=" +
                    std::to_string(animation.observedSourceTexture) +
                    " target=" +
                    std::to_string(animation.observedSourceTarget) +
                    " textureSize=" +
                    std::to_string(static_cast<int>(animation.observedSourceSize.x)) +
                    "x" +
                    std::to_string(static_cast<int>(animation.observedSourceSize.y)) +
                    " aspectMismatch=" +
                    std::to_string(animation.observedAspectMismatch) +
                    " pendingSizeAck=" +
                    std::string(animation.observedPendingSizeAck ? "yes" : "no") +
                    " pendingSizeAcks=" +
                    std::to_string(animation.observedPendingSizeAckCount) +
                    " surfaceDepth=" +
                    std::to_string(animation.sourceSurfaceDepth) +
                    " surfaceCandidates=" +
                    std::to_string(animation.sourceCandidateCount)
                );
                cancelAnimation("no usable target texture reached the render pass");
            } else if (closing) {
                damageExpandedBox(animation.box);
            } else {
                const CBox liveBox = currentWindowRenderBox(window);
                damageExpandedBox(animation.box);
                damageExpandedBox(liveBox);
                animation.box = liveBox;
                animation.rounding = window->rounding();
            }
        } else {
            const float elapsed = std::chrono::duration<float>(
                now - animation.startTime
            ).count();
            const float duration = animationDurationSeconds(animation, *effect);

            if (!animation.sawPass && elapsed >= kPassWaitTimeoutSeconds) {
                cancelAnimation("no usable target texture reached the render pass");
            } else if (elapsed >= duration && animation.terminalFrameQueued) {
                switch (animation.mode) {
                    case EWindowAnimationMode::AutomaticOpen:
                        cancelAnimation("open complete after terminal live frame");
                        break;
                    case EWindowAnimationMode::AutomaticClose:
                        cancelAnimation("close complete; native tiled reflow resumed");
                        break;
                    case EWindowAnimationMode::ManualCycle:
                        cancelAnimation("cycle complete after terminal live frame");
                        break;
                }
            } else if (closing) {
                damageExpandedBox(animation.box);
            } else {
                const CBox liveBox = currentWindowRenderBox(window);
                damageExpandedBox(animation.box);
                damageExpandedBox(liveBox);
                animation.box = liveBox;
                animation.rounding = window->rounding();
            }
        }
    }

    const int timeout = g_pHyprRenderer->m_mostHzMonitor
        ? static_cast<int>(1000.0 / g_pHyprRenderer->m_mostHzMonitor->m_refreshRate)
        : 16;
    wl_event_source_timer_update(g_state->tick, std::max(timeout, 1));
    return 0;
}

void onWindowClose(PHLWINDOW window) {
    if (!g_state || !window)
        return;

    if (g_state->animation.active && g_state->animation.window.lock() == window)
        cancelAnimation("target window began closing", false);

    if (!g_state->automaticCloseEnabled)
        return;

    const std::string_view windowClass = effectiveWindowClass(window);
    const auto& effectPool = automaticCloseEffectsForWindow(
        g_state->effectConfig,
        windowClass,
        window->m_title
    );
    const std::string_view effectName = chooseWindowEffect(
        effectPool,
        g_state->effectRandom(),
        requiredCapabilitiesForTarget(GL_TEXTURE_2D, window->rounding() > 0.0F)
    );
    if (effectName == kNoWindowEffect) {
        appendDiagnostic(
            "automatic close skipped: reason=assignment resolved to none or class is excluded"
        );
        return;
    }

    if (g_state->animation.active) {
        appendDiagnostic(
            "automatic close skipped: reason=another animation is active"
        );
        return;
    }

    const SWindowEffectSpec* effect = findWindowEffect(effectName);
    if (effect == nullptr) {
        appendDiagnostic(
            "automatic close skipped: reason=assigned effect is missing"
        );
        return;
    }

    std::string reason;
    if (!automaticCloseEligibility(window, *effect, reason)) {
        appendDiagnostic(
            "automatic close skipped: reason=" + reason
        );
        return;
    }

    const auto monitor = window->m_monitor.lock();
    const CBox closingBox = currentWindowRenderBox(window);
    if (!monitor || !validBox(closingBox)) {
        appendDiagnostic(
            "automatic close skipped: reason=target monitor or geometry is unavailable"
        );
        return;
    }

    SRetainedCloseFrame closeFrame;
    std::uint32_t closeSurfaceDepth = 0;
    std::size_t closeSurfaceCandidates = 0;
    if (!captureCloseFrame(
            window,
            closeFrame,
            reason,
            &closeSurfaceDepth,
            &closeSurfaceCandidates
        )) {
        appendDiagnostic(
            "automatic close skipped: reason=owned closing snapshot failed: " + reason
        );
        return;
    }
    if (!effectSupportsTarget(*effect, GL_TEXTURE_2D, window->rounding() > 0.0F)) {
        appendDiagnostic(
            "automatic close skipped: reason=effect does not support the owned 2D snapshot"
        );
        return;
    }

    std::vector<SHeldReflowWindow> heldReflowWindows;
    if (!captureTiledReflowHold(
            window,
            monitor,
            heldReflowWindows,
            reason
        )) {
        appendDiagnostic(
            "automatic close skipped: reason=survivor scene capture failed: " + reason
        );
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    g_state->animation = {
        .window = window,
        .monitor = monitor,
        .effect = effect,
        .candidateEffects = effectPool,
        .mode = EWindowAnimationMode::AutomaticClose,
        .closeFrame = std::move(closeFrame),
        .heldReflowWindows = std::move(heldReflowWindows),
        .reflowHoldApplied = false,
        .box = closingBox,
        .rounding = window->rounding(),
        .windowClass = std::string(windowClass),
        .armedTime = now,
        .startTime = now,
        .sourceSurfaceDepth = closeSurfaceDepth,
        .sourceCandidateCount = closeSurfaceCandidates,
        .active = true,
        .started = true,
        .sawPass = false,
        .terminalFrameQueued = false,
    };

    appendDiagnostic(
        "automatic close armed: effect=" + std::string(effect->name) +
        " source=owned-surface-copy target=" +
        std::to_string(GL_TEXTURE_2D) + " box=" +
        std::to_string(static_cast<int>(closingBox.width)) + "x" +
        std::to_string(static_cast<int>(closingBox.height)) +
        " heldWindows=" +
        std::to_string(g_state->animation.heldReflowWindows.size()) +
        " surfaceDepth=" + std::to_string(closeSurfaceDepth) +
        " surfaceCandidates=" + std::to_string(closeSurfaceCandidates)
    );
    damageExpandedBox(closingBox);
}

void onWorkspace(PHLWORKSPACE) {
    if (g_state && g_state->animation.active)
        cancelAnimation("workspace changed");
}

std::string controlCommand(eHyprCtlOutputFormat format, std::string request) {
    (void)format;

    std::istringstream stream(std::move(request));
    std::string command;
    std::string subcommand;
    stream >> command >> subcommand;

    if (subcommand == "test") {
        std::string effectName = "void";
        stream >> effectName;
        return armFocusedWindow(effectName);
    }
    if (subcommand == "status")
        return g_state && g_state->animation.active ? "active" : "idle";
    if (subcommand == "cancel") {
        cancelAnimation("cancelled by user");
        return "ok";
    }
    if (subcommand == "effects")
        return currentWindowEffectsSummary();
    if (subcommand == "config") {
        std::string action;
        stream >> action;
        if (action.empty() || action == "status")
            return currentWindowEffectConfigStatus();
        if (action == "path") {
            if (!g_state || g_state->effectConfigPath.empty())
                return "unavailable";
            return g_state->effectConfigPath.string();
        }
        if (action == "reload")
            return reloadWindowEffectConfig(false);
        return "usage: realmheart-fx config status|path|reload";
    }
    if (subcommand == "auto-open") {
        std::string action;
        stream >> action;
        if (action.empty() || action == "status")
            return g_state && g_state->automaticOpenEnabled ? "enabled" : "disabled";
        if (action == "on") {
            g_state->automaticOpenEnabled = true;
            appendDiagnostic("automatic open enabled");
            return "ok";
        }
        if (action == "off") {
            g_state->automaticOpenEnabled = false;
            if (g_state->animation.active &&
                g_state->animation.mode == EWindowAnimationMode::AutomaticOpen) {
                cancelAnimation("automatic open disabled by user");
            }
            appendDiagnostic("automatic open disabled");
            return "ok";
        }
        return "usage: realmheart-fx auto-open on|off|status";
    }
    if (subcommand == "auto-close") {
        std::string action;
        stream >> action;
        if (action.empty() || action == "status")
            return g_state && g_state->automaticCloseEnabled ? "enabled" : "disabled";
        if (action == "on") {
            g_state->automaticCloseEnabled = true;
            appendDiagnostic(
                "automatic close enabled: owned composed-snapshot path"
            );
            return "ok";
        }
        if (action == "off") {
            g_state->automaticCloseEnabled = false;
            if (g_state->animation.active &&
                g_state->animation.mode == EWindowAnimationMode::AutomaticClose) {
                cancelAnimation("automatic close disabled by user");
            }
            appendDiagnostic("automatic close disabled");
            return "ok";
        }
        return "usage: realmheart-fx auto-close on|off|status";
    }

    return "usage: realmheart-fx test [effect]|status|cancel|effects|"
           "config status|path|reload|auto-open on|off|status|"
           "auto-close on|off|status";
}

void cleanupPlugin() noexcept {
    try {
        g_listeners.clear();
    } catch (...) {
    }

    if (!g_state) {
        g_handle = nullptr;
        return;
    }

    try {
        cancelAnimation("plugin cleanup");
    } catch (...) {
    }

    try {
        if (g_state->command)
            HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_state->command);
    } catch (...) {
    }
    if (g_state->tick != nullptr) {
        wl_event_source_remove(g_state->tick);
        g_state->tick = nullptr;
    }

    try {
        if (g_pHyprRenderer != nullptr) {
            g_pHyprRenderer->m_renderPass.removeAllOfType(
                "CRealmheartEffectPassElement"
            );
        }
    } catch (...) {
    }

    try {
        if (g_pHyprOpenGL != nullptr) {
            g_pHyprOpenGL->makeEGLCurrent();
            for (auto& effect : g_state->effects)
                effect.destroy();
            g_state->sceneBlitShader.destroy();
        }
    } catch (...) {
    }
    g_state->effects.clear();
    g_state.reset();
    g_handle = nullptr;
}

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;
    try {
        appendDiagnostic("Realmheart FX target-only pluginInit");

        const auto runtime = HyprlandAPI::getHyprlandVersion(handle);
        if (runtime.hash != GIT_COMMIT_HASH) {
            throw std::runtime_error(
                "Realmheart FX was built for Hyprland " + std::string(GIT_COMMIT_HASH) +
                " but the running compositor is " + runtime.hash
            );
        }
        if (runtime.dirty) {
            throw std::runtime_error(
                "Realmheart FX refuses to load against a dirty Hyprland build"
            );
        }

        const auto registry = loadWindowEffectRegistry(defaultWindowEffectAssetRoot());
        if (!registry.success)
            throw std::runtime_error("Realmheart FX effect registry failed: " + registry.error);

        appendDiagnostic(
            "effect manifests loaded: count=" + std::to_string(registry.loadedEffects) +
            " effects=" + currentWindowEffectsSummary()
        );

        g_state = makeUnique<SPluginState>();
        initialiseEffects();
        (void)reloadWindowEffectConfig(true);

        auto& events = Event::bus()->m_events;
        g_listeners.push_back(events.window.open.listen(onWindowOpen));
        g_listeners.push_back(events.window.close.listen(onWindowClose));
        g_listeners.push_back(events.render.stage.listen(onRenderStage));
        g_listeners.push_back(events.workspace.active.listen(onWorkspace));

        g_state->command = HyprlandAPI::registerHyprCtlCommand(
            g_handle,
            SHyprCtlCommand{
                .name = kCommandName,
                .exact = false,
                .fn = controlCommand,
            }
        );
        if (!g_state->command)
            throw std::runtime_error("failed to register Realmheart FX hyprctl command");

        g_state->tick = wl_event_loop_add_timer(
            g_pCompositor->m_wlEventLoop,
            &onTick,
            nullptr
        );
        if (!g_state->tick)
            throw std::runtime_error("failed to create Realmheart FX frame timer");
        wl_event_source_timer_update(g_state->tick, 1);

        appendDiagnostic("automatic open policy enabled");
        appendDiagnostic("automatic close policy disabled by default; explicit opt-in required");
        appendDiagnostic(
            "lifecycle invariant: tiled close replays frozen survivor pixels; live clients stay hidden until native reflow resumes"
        );

        return {
            .name = "Realmheart FX",
            .description = "Realmheart target-only Hyprland window transitions",
            .author = "Zahed",
            .version = "0.10.14-realmheart",
        };
    } catch (...) {
        cleanupPlugin();
        throw;
    }
}

APICALL EXPORT void PLUGIN_EXIT() {
    cleanupPlugin();
}
