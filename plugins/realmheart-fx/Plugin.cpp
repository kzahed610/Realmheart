// SPDX-License-Identifier: GPL-3.0-or-later
// Compositor render-pass integration adapted from  hyprfx (commit
// d680dabdd2d9362626ecedcad9bd396508163468), itself derived from xhos/hyprfx.

#define WLR_USE_UNSTABLE

#include "RealmheartEffectPassElement.hpp"
#include "WindowEffectConfig.hpp"
#include "WindowEffectPolicy.hpp"
#include "WindowEffectRegistry.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
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
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Render::GL;

namespace {

constexpr float kTextureWaitTimeoutSeconds = 0.60F;
constexpr float kAutomaticOpenDurationScale = 0.82F;
constexpr float kAutomaticCloseDurationScale = 0.82F;
constexpr const char* kDiagnosticLog = "/tmp/realmheart-fx.log";
constexpr const char* kCommandName = "realmheart-fx";

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
uniform float opacity;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = texture(tex, v_texcoord) * clamp(opacity, 0.0, 1.0);
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

enum class EWindowAnimationMode {
    ManualCycle,
    AutomaticOpen,
    AutomaticClose,
};

struct SRetainedCloseFrame {
    GLuint texture = 0;
    GLuint framebuffer = 0;

    void destroy() {
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

struct SFrozenSceneWindow {
    PHLWINDOWREF window;
    CBox box{};
    float rounding = 0.0F;
    SRetainedCloseFrame frame{};
};

struct SWindowAnimation {
    PHLWINDOWREF window;
    PHLMONITORREF monitor;
    const SWindowEffectSpec* effect = nullptr;
    EWindowAnimationMode mode = EWindowAnimationMode::ManualCycle;
    CBox box{};
    float rounding = 0.0F;
    std::string windowClass;
    std::chrono::steady_clock::time_point armedTime{};
    std::chrono::steady_clock::time_point startTime{};
    SRetainedCloseFrame closeFrame{};
    SRetainedCloseFrame backdropFrame{};
    CBox sceneBackdropBox{};
    std::vector<SFrozenSceneWindow> frozenSceneWindows{};
    bool freezeTiledScene = false;
    bool active = false;
    bool started = false;
    bool sawPass = false;
    bool terminalFrameQueued = false;
    bool backdropCaptureQueued = false;
    bool backdropCaptureFailed = false;
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

HANDLE g_handle = nullptr;
UP<SPluginState> g_state;
std::vector<Hyprutils::Signal::CHyprSignalListener> g_listeners;

void appendDiagnostic(const std::string& message) {
    std::ofstream stream(kDiagnosticLog, std::ios::app);
    if (stream)
        stream << message << '\n';
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

class CRealmheartBackdropCapturePassElement final : public IPassElement {
  public:
    struct SData {
        CBox box;
        SRetainedCloseFrame* destination = nullptr;
        bool* failed = nullptr;
    };

    explicit CRealmheartBackdropCapturePassElement(SData data)
        : m_data(std::move(data)) {}

    std::vector<UP<IPassElement>> draw() override {
        if (m_data.destination == nullptr || m_data.failed == nullptr ||
            m_data.box.width <= 0.0 || m_data.box.height <= 0.0) {
            return {};
        }
        if (m_data.destination->texture != 0)
            return {};

        const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!monitor) {
            *m_data.failed = true;
            return {};
        }

        CBox renderBox = m_data.box;
        renderBox.translate(-monitor->m_position).scale(monitor->m_scale).round();

        GLint viewport[4]{};
        GLint previousReadFramebuffer = 0;
        GLint previousDrawFramebuffer = 0;
        GLint previousActiveTexture = 0;
        GLint previousTexture2D = 0;
        GLint previousScissorBox[4]{};
        const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
        glGetIntegerv(GL_SCISSOR_BOX, previousScissorBox);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture2D);

        const int sourceX = static_cast<int>(std::lround(renderBox.x));
        const int sourceTop = static_cast<int>(std::lround(renderBox.y));
        const int sourceWidth = static_cast<int>(std::lround(renderBox.width));
        const int sourceHeight = static_cast<int>(std::lround(renderBox.height));
        const int sourceBottom = viewport[3] - (sourceTop + sourceHeight);

        if (sourceWidth <= 0 || sourceHeight <= 0 || sourceX < 0 ||
            sourceBottom < 0 || sourceX + sourceWidth > viewport[2] ||
            sourceBottom + sourceHeight > viewport[3]) {
            *m_data.failed = true;
            return {};
        }

        glDisable(GL_SCISSOR_TEST);

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
            sourceWidth,
            sourceHeight,
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
            glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (captured) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, previousDrawFramebuffer);
            while (glGetError() != GL_NO_ERROR) {
            }
            glBlitFramebuffer(
                sourceX,
                sourceBottom,
                sourceX + sourceWidth,
                sourceBottom + sourceHeight,
                0,
                0,
                sourceWidth,
                sourceHeight,
                GL_COLOR_BUFFER_BIT,
                GL_NEAREST
            );
            captured = glGetError() == GL_NO_ERROR;
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
        glScissor(
            previousScissorBox[0],
            previousScissorBox[1],
            previousScissorBox[2],
            previousScissorBox[3]
        );
        if (scissorEnabled)
            glEnable(GL_SCISSOR_TEST);
        else
            glDisable(GL_SCISSOR_TEST);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture2D));
        glActiveTexture(static_cast<GLenum>(previousActiveTexture));

        if (!captured) {
            retained.destroy();
            *m_data.failed = true;
            return {};
        }

        m_data.destination->destroy();
        *m_data.destination = retained;
        appendDiagnostic(
            "closing backdrop captured: box=" + std::to_string(sourceWidth) + "x" +
            std::to_string(sourceHeight)
        );
        return {};
    }

    bool needsLiveBlur() override {
        return false;
    }

    bool needsPrecomputeBlur() override {
        return false;
    }

    std::optional<CBox> boundingBox() override {
        const auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!monitor)
            return std::nullopt;

        CBox box = m_data.box;
        return box.translate(-monitor->m_position).expand(2);
    }

    bool disableSimplification() override {
        return true;
    }

    const char* passName() override {
        return "CRealmheartBackdropCapturePassElement";
    }

    ePassElementType type() override {
        return EK_CUSTOM;
    }

  private:
    SData m_data;
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
    const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertex);
    const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragment);
    const GLuint program = glCreateProgram();

    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    glDetachShader(program, vertexShader);
    glDetachShader(program, fragmentShader);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE)
        return program;

    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    glDeleteProgram(program);
    throw std::runtime_error("Realmheart FX program linking failed: " + log);
}

std::string readEffectShader(const SWindowEffectSpec& effect) {
    const std::filesystem::path shaderPath =
        std::filesystem::path{REALMHEART_EFFECT_ASSET_DIR} / effect.fragmentShaderAsset;
    std::ifstream stream(shaderPath, std::ios::binary);
    if (!stream) {
        throw std::runtime_error(
            "could not read " + std::string(effect.displayName) +
            " shader at " + shaderPath.string()
        );
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string externalVariant(std::string fragment) {
    constexpr const char* version = "#version 300 es\n";
    if (const auto position = fragment.find(version); position != std::string::npos) {
        fragment.insert(
            position + std::char_traits<char>::length(version),
            "#extension GL_OES_EGL_image_external_essl3 : require\n"
        );
    }

    constexpr const char* sampler2d = "uniform sampler2D tex;";
    if (const auto position = fragment.find(sampler2d); position != std::string::npos) {
        fragment.replace(
            position,
            std::char_traits<char>::length(sampler2d),
            "uniform samplerExternalOES tex;"
        );
    }

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

    for (const auto& spec : windowEffectSpecs()) {
        if (windowEffectIsNone(spec) || spec.fragmentShaderAsset.empty())
            continue;

        const std::string fragment = readEffectShader(spec);
        SCompiledWindowEffect compiled{
            .spec = &spec,
        };
        compiled.shaders[0].program = createProgram(kVertexShader, fragment);
        fillLocations(compiled.shaders[0]);

        if (windowEffectSupports(spec, EWindowEffectCapability::ExternalTexture)) {
            compiled.shaders[1].program = createProgram(
                kVertexShader,
                externalVariant(fragment)
            );
            fillLocations(compiled.shaders[1]);
        }

        g_state->effects.push_back(std::move(compiled));
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

SP<Render::ITexture> surfaceTexture(const PHLWINDOW& window) {
    SP<CWLSurfaceResource> surface;

    if (!window->m_xdgSurface.expired()) {
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

    if (const auto texture = stateTexture(surface))
        return texture;

    SP<Render::ITexture> largest;
    for (auto& weakSubsurface : surface->m_subsurfaces) {
        const auto subsurface = weakSubsurface.lock();
        if (!subsurface || subsurface->m_surface.expired())
            continue;

        const auto texture = stateTexture(subsurface->m_surface.lock());
        if (!texture)
            continue;

        if (!largest || texture->m_size.x * texture->m_size.y >
                largest->m_size.x * largest->m_size.y) {
            largest = texture;
        }
    }

    return largest;
}

std::string_view effectiveWindowClass(const PHLWINDOW& window) noexcept {
    if (!window)
        return {};
    if (!window->m_class.empty())
        return window->m_class;
    return window->m_initialClass;
}

GLenum textureTarget(const SP<Render::ITexture>& texture) {
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

CBox currentWindowRenderBox(const PHLWINDOW& window) {
    if (!window)
        return {};

    const auto position = window->m_realPosition->value();
    const auto size = window->m_realSize->value();
    return CBox{
        position.x,
        position.y,
        std::max(size.x, 5.0),
        std::max(size.y, 5.0),
    };
}

void settleWindowGeometry(const PHLWINDOW& window) {
    if (!window)
        return;

    // Realmheart replaces Hyprland's built-in open transform. Finish any
    // already-armed geometry animation so the effect quad and the real window
    // share exactly the same box at hand-off.
    window->m_realPosition->warp();
    window->m_realSize->warp();
}

bool copySnapshotCrop(
    const auto& snapshot,
    const PHLMONITOR& monitor,
    const CBox& box,
    SRetainedCloseFrame& destination,
    std::string& reason
) {
    if (!snapshot || !monitor) {
        reason = "snapshot or monitor is unavailable";
        return false;
    }

    const auto sourceTexture = snapshot->getTexture();
    if (!sourceTexture || sourceTexture->m_texID == 0) {
        reason = "snapshot has no usable texture";
        return false;
    }
    if (textureTarget(sourceTexture) != GL_TEXTURE_2D) {
        reason = "snapshot is not a 2D texture";
        return false;
    }

    const int framebufferWidth = static_cast<int>(sourceTexture->m_size.x);
    const int framebufferHeight = static_cast<int>(sourceTexture->m_size.y);
    if (framebufferWidth <= 0 || framebufferHeight <= 0 ||
        monitor->m_size.x <= 0.0 || monitor->m_size.y <= 0.0) {
        reason = "snapshot has invalid dimensions";
        return false;
    }

    const double scaleX = static_cast<double>(framebufferWidth) / monitor->m_size.x;
    const double scaleY = static_cast<double>(framebufferHeight) / monitor->m_size.y;
    const int sourceX = static_cast<int>(std::lround(
        (box.x - monitor->m_position.x) * scaleX
    ));
    const int sourceTop = static_cast<int>(std::lround(
        (box.y - monitor->m_position.y) * scaleY
    ));
    const int sourceWidth = static_cast<int>(std::lround(box.width * scaleX));
    const int sourceHeight = static_cast<int>(std::lround(box.height * scaleY));

    if (sourceWidth <= 0 || sourceHeight <= 0 || sourceX < 0 || sourceTop < 0 ||
        sourceX + sourceWidth > framebufferWidth ||
        sourceTop + sourceHeight > framebufferHeight) {
        reason = "snapshot crop is outside the monitor framebuffer";
        return false;
    }

    GLint previousReadFramebuffer = 0;
    GLint previousDrawFramebuffer = 0;
    GLint previousActiveTexture = 0;
    GLint previousTexture2D = 0;
    GLint previousViewport[4]{};
    GLint previousScissorBox[4]{};
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_SCISSOR_BOX, previousScissorBox);
    glDisable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture2D);

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
        sourceWidth,
        sourceHeight,
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

    bool captured = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (captured) {
        snapshot->bind();
        GLint sourceFramebuffer = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &sourceFramebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, retained.framebuffer);

        const int sourceBottom = framebufferHeight - (sourceTop + sourceHeight);
        while (glGetError() != GL_NO_ERROR) {
        }
        glBlitFramebuffer(
            sourceX,
            sourceBottom,
            sourceX + sourceWidth,
            sourceBottom + sourceHeight,
            0,
            0,
            sourceWidth,
            sourceHeight,
            GL_COLOR_BUFFER_BIT,
            GL_NEAREST
        );
        captured = glGetError() == GL_NO_ERROR;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
    glViewport(
        previousViewport[0],
        previousViewport[1],
        previousViewport[2],
        previousViewport[3]
    );
    glScissor(
        previousScissorBox[0],
        previousScissorBox[1],
        previousScissorBox[2],
        previousScissorBox[3]
    );
    if (scissorEnabled)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture2D));
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));

    if (!captured) {
        retained.destroy();
        reason = "failed to copy snapshot texture";
        return false;
    }

    destination.destroy();
    destination = retained;
    return true;
}

void destroyFrozenSceneWindows(std::vector<SFrozenSceneWindow>& windows) {
    for (auto& window : windows)
        window.frame.destroy();
    windows.clear();
}

bool captureFrozenSceneWindows(
    const PHLWINDOW& closingWindow,
    const PHLMONITOR& monitor,
    std::vector<SFrozenSceneWindow>& destination,
    std::string& reason
) {
    if (!closingWindow || !monitor) {
        reason = "closing window or monitor is unavailable";
        return false;
    }

    std::vector<PHLWINDOW> tiledWindows;
    PHLWINDOW focusedTiledWindow;
    const auto focusedWindow = Desktop::focusState()->window();

    for (const auto& candidate : g_pCompositor->m_windows) {
        if (!candidate || candidate == closingWindow || !candidate->m_isMapped ||
            candidate->isHidden() || candidate->m_isFloating ||
            candidate->m_workspace != closingWindow->m_workspace ||
            candidate->m_monitor.lock() != monitor ||
            !g_pHyprRenderer->shouldRenderWindow(candidate, monitor)) {
            continue;
        }

        if (candidate == focusedWindow)
            focusedTiledWindow = candidate;
        else
            tiledWindows.push_back(candidate);
    }

    if (focusedTiledWindow)
        tiledWindows.push_back(focusedTiledWindow);

    g_pHyprOpenGL->makeEGLCurrent();
    for (const auto& candidate : tiledWindows) {
        const auto previousSnapshot = candidate->m_snapshotFB;
        candidate->m_snapshotFB = {};
        g_pHyprRenderer->makeSnapshot(candidate);
        const auto snapshot = candidate->m_snapshotFB;
        candidate->m_snapshotFB = previousSnapshot;

        if (!snapshot || !snapshot->getTexture()) {
            reason = "failed to create a pre-reflow snapshot for " +
                std::string(effectiveWindowClass(candidate));
            destroyFrozenSceneWindows(destination);
            return false;
        }

        SFrozenSceneWindow frozen{
            .window = candidate,
            .box = currentWindowRenderBox(candidate),
            .rounding = candidate->rounding(),
            .frame = {},
        };
        if (!copySnapshotCrop(snapshot, monitor, frozen.box, frozen.frame, reason)) {
            reason = "failed to retain pre-reflow window " +
                std::string(effectiveWindowClass(candidate)) + ": " + reason;
            destroyFrozenSceneWindows(destination);
            return false;
        }

        destination.push_back(std::move(frozen));
    }

    return true;
}

bool captureCloseFrame(
    const PHLWINDOW& window,
    SWindowAnimation& animation,
    std::string& reason
) {
    if (!window || !window->m_snapshotFB) {
        reason = "Hyprland snapshot is not ready";
        return false;
    }

    const auto monitor = window->m_monitor.lock();
    if (!monitor) {
        reason = "closing window monitor disappeared";
        return false;
    }

    if (!copySnapshotCrop(
            window->m_snapshotFB,
            monitor,
            animation.box,
            animation.closeFrame,
            reason
        )) {
        reason = "failed to retain Hyprland's closing snapshot: " + reason;
        return false;
    }

    animation.monitor = monitor;
    return true;
}

void applyAlphaNow(const PHLWINDOW& window) {
    window->updateDecorationValues();
    auto& alpha = window->alpha(Desktop::View::WINDOW_ALPHA_ACTIVE);
    alpha->setValueAndWarp(alpha->goal());
}

void setWindowHidden(const PHLWINDOW& window) {
    const auto hidden = Desktop::Types::SAlphaValue{
        .alpha = 0.0F,
        .overridden = true,
    };

    window->m_ruleApplicator->alpha().set(
        hidden,
        Desktop::Types::PRIORITY_SET_PROP
    );
    window->m_ruleApplicator->alphaInactive().set(
        hidden,
        Desktop::Types::PRIORITY_SET_PROP
    );
    window->m_ruleApplicator->alphaFullscreen().set(
        hidden,
        Desktop::Types::PRIORITY_SET_PROP
    );
    window->m_ruleApplicator->noAnim().set(
        true,
        Desktop::Types::PRIORITY_SET_PROP
    );
    applyAlphaNow(window);
}

void clearWindowHidden(const PHLWINDOW& window) {
    if (!window || !window->m_ruleApplicator)
        return;

    window->m_ruleApplicator->alpha().unset(Desktop::Types::PRIORITY_SET_PROP);
    window->m_ruleApplicator->alphaInactive().unset(Desktop::Types::PRIORITY_SET_PROP);
    window->m_ruleApplicator->alphaFullscreen().unset(Desktop::Types::PRIORITY_SET_PROP);
    window->m_ruleApplicator->noAnim().unset(Desktop::Types::PRIORITY_SET_PROP);
    applyAlphaNow(window);

    g_pHyprRenderer->damageBox(currentWindowRenderBox(window));
}

void damageExpandedBox(CBox box) {
    box.expand(2);
    g_pHyprRenderer->damageBox(box);
}

CBox closeSceneBackdropBox(
    const CBox& closingBox,
    const std::vector<SFrozenSceneWindow>& frozenWindows
) {
    double left = closingBox.x;
    double top = closingBox.y;
    double right = closingBox.x + closingBox.width;
    double bottom = closingBox.y + closingBox.height;

    for (const auto& frozen : frozenWindows) {
        left = std::min(left, frozen.box.x);
        top = std::min(top, frozen.box.y);
        right = std::max(right, frozen.box.x + frozen.box.width);
        bottom = std::max(bottom, frozen.box.y + frozen.box.height);
    }

    return CBox{
        left,
        top,
        std::max(right - left, 1.0),
        std::max(bottom - top, 1.0),
    };
}

void damageCloseAnimationRegion(const SWindowAnimation& animation) {
    damageExpandedBox(
        animation.sceneBackdropBox.width > 0.0 &&
                animation.sceneBackdropBox.height > 0.0
            ? animation.sceneBackdropBox
            : animation.box
    );
}

void restartFrozenSceneReflow(SWindowAnimation& animation) {
    if (!animation.freezeTiledScene)
        return;

    const auto monitor = animation.monitor.lock();
    std::size_t restarted = 0;
    for (const auto& frozen : animation.frozenSceneWindows) {
        const auto window = frozen.window.lock();
        if (!window || !window->m_isMapped || window->isHidden() ||
            window->m_isFloating || window->isFullscreen() ||
            window->m_monitor.lock() != monitor ||
            !window->m_realPosition || !window->m_realSize) {
            continue;
        }

        // Hyprland has already recalculated the final tiled geometry while the
        // retained old scene covered it. Preserve those final goals, return the
        // live window to its old tile, then assign the goals again so Hyprland's
        // own window animation performs the real resize after Void completes.
        const Vector2D finalPosition = window->m_realPosition->goal();
        const Vector2D finalSize = window->m_realSize->goal();
        if (finalSize.x < 5.0 || finalSize.y < 5.0)
            continue;

        const Vector2D oldPosition{frozen.box.x, frozen.box.y};
        const Vector2D oldSize{frozen.box.width, frozen.box.height};

        window->m_realPosition->setValueAndWarp(oldPosition);
        window->m_realSize->setValueAndWarp(oldSize);
        *window->m_realPosition = finalPosition;
        *window->m_realSize = finalSize;

        g_pHyprRenderer->damageBox(frozen.box);
        g_pHyprRenderer->damageBox(CBox{finalPosition, finalSize});
        ++restarted;
    }

    appendDiagnostic(
        "close reflow restarted: windows=" + std::to_string(restarted)
    );
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

void cancelAnimation(const std::string& reason) {
    if (!g_state || !g_state->animation.active)
        return;

    const auto mode = g_state->animation.mode;
    const auto window = g_state->animation.window.lock();
    if (mode != EWindowAnimationMode::AutomaticClose)
        clearWindowHidden(window);
    else
        damageCloseAnimationRegion(g_state->animation);
    g_pHyprRenderer->m_renderPass.removeAllOfType("CRealmheartBackdropCapturePassElement");
    g_pHyprRenderer->m_renderPass.removeAllOfType("CRealmheartEffectPassElement");
    g_state->animation.closeFrame.destroy();
    g_state->animation.backdropFrame.destroy();
    destroyFrozenSceneWindows(g_state->animation.frozenSceneWindows);
    g_state->animation = {};
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
    if (window->isFullscreen())
        return "fullscreen windows are excluded";
    if (window->isX11OverrideRedirect())
        return "override-redirect windows are excluded";
    if (!window->m_ruleApplicator)
        return "focused window has no rule applicator";

    if (g_state->animation.active)
        cancelAnimation("replaced by manual test");

    const std::string_view windowClass = effectiveWindowClass(window);
    const auto now = std::chrono::steady_clock::now();
    setWindowHidden(window);
    g_state->animation = {
        .window = window,
        .monitor = window->m_monitor.lock(),
        .effect = effect,
        .mode = EWindowAnimationMode::ManualCycle,
        .windowClass = std::string(windowClass),
        .armedTime = now,
        .startTime = now,
        .closeFrame = {},
        .backdropFrame = {},
        .sceneBackdropBox = {},
        .frozenSceneWindows = {},
        .freezeTiledScene = false,
        .active = true,
        .started = true,
        .sawPass = false,
        .terminalFrameQueued = false,
    };

    appendDiagnostic(
        "manual test armed: effect=" + std::string(effect->name) +
        " class=" + window->m_class + " title=" + window->m_title
    );

    if (g_handle) {
        HyprlandAPI::addNotification(
            g_handle,
            "[Realmheart FX] " + std::string(effect->displayName) +
                " test armed on " + window->m_class,
            CHyprColor{0.55F, 0.38F, 0.95F, 1.0F},
            3000.0F
        );
    }

    g_pHyprRenderer->damageWindow(window, true);
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
    if (window->isFullscreen()) {
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
    if (window->isFullscreen()) {
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
            "automatic open skipped: class=" + std::string(windowClass) +
            " liveClass=" + window->m_class +
            " initialClass=" + window->m_initialClass +
            " reason=assignment resolved to none or class is excluded"
        );
        return;
    }

    const SWindowEffectSpec* effect = findWindowEffect(effectName);
    if (effect == nullptr) {
        appendDiagnostic(
            "automatic open skipped: class=" + std::string(windowClass) +
            " reason=assigned effect is missing"
        );
        return;
    }

    if (g_state->animation.active) {
        appendDiagnostic(
            "automatic open skipped: class=" + std::string(windowClass) +
            " reason=another animation is active"
        );
        return;
    }

    std::string reason;
    if (!automaticOpenEligibility(window, *effect, reason)) {
        appendDiagnostic(
            "automatic open skipped: class=" + std::string(windowClass) +
            " reason=" + reason
        );
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    settleWindowGeometry(window);
    setWindowHidden(window);
    g_state->animation = {
        .window = window,
        .monitor = window->m_monitor.lock(),
        .effect = effect,
        .mode = EWindowAnimationMode::AutomaticOpen,
        .windowClass = std::string(windowClass),
        .armedTime = now,
        .startTime = {},
        .closeFrame = {},
        .backdropFrame = {},
        .sceneBackdropBox = {},
        .frozenSceneWindows = {},
        .freezeTiledScene = false,
        .active = true,
        .started = false,
        .sawPass = false,
        .terminalFrameQueued = false,
    };

    appendDiagnostic(
        "automatic open armed: effect=" + std::string(effect->name) +
        " class=" + std::string(windowClass) + " title=" + window->m_title
    );
    g_pHyprRenderer->damageWindow(window, true);
}

void onRenderStage(eRenderStage stage) {
    if (!g_state || !g_state->animation.active)
        return;

    auto& animation = g_state->animation;
    if (stage == RENDER_PRE_WINDOWS) {
        if (animation.mode != EWindowAnimationMode::AutomaticClose ||
            animation.backdropFrame.texture != 0 ||
            animation.backdropCaptureQueued ||
            animation.backdropCaptureFailed) {
            return;
        }

        const auto currentMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        const auto targetMonitor = animation.monitor.lock();
        if (currentMonitor && targetMonitor && currentMonitor == targetMonitor) {
            g_pHyprRenderer->m_renderPass.add(
                makeUnique<CRealmheartBackdropCapturePassElement>(
                    CRealmheartBackdropCapturePassElement::SData{
                        .box = animation.sceneBackdropBox,
                        .destination = &animation.backdropFrame,
                        .failed = &animation.backdropCaptureFailed,
                    }
                )
            );
            animation.backdropCaptureQueued = true;
        }
        return;
    }

    if (stage != RENDER_POST_WINDOWS)
        return;
    const SWindowEffectSpec* effect = animation.effect;
    SCompiledWindowEffect* compiled = compiledEffect(effect);
    if (effect == nullptr || compiled == nullptr) {
        cancelAnimation("selected effect became unavailable");
        return;
    }

    const auto window = animation.window.lock();
    if (animation.mode != EWindowAnimationMode::AutomaticClose &&
        (!window || !window->m_isMapped)) {
        cancelAnimation("window disappeared");
        return;
    }

    const auto currentMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    const auto targetMonitor = animation.mode == EWindowAnimationMode::AutomaticClose
        ? animation.monitor.lock()
        : window->m_monitor.lock();
    if (targetMonitor && currentMonitor && targetMonitor != currentMonitor)
        return;

    SP<Render::ITexture> texture;
    GLenum target = 0;
    CBox effectBox;
    float rounding = 0.0F;

    if (animation.mode == EWindowAnimationMode::AutomaticClose) {
        if (animation.backdropCaptureFailed) {
            cancelAnimation("failed to capture the pre-window backdrop");
            return;
        }
        if (animation.backdropFrame.texture == 0)
            return;

        if (!animation.started) {
            if (!window) {
                cancelAnimation("closing window disappeared before snapshot capture");
                return;
            }

            std::string reason;
            if (!captureCloseFrame(window, animation, reason)) {
                if (reason != "Hyprland snapshot is not ready")
                    cancelAnimation(reason);
                return;
            }

            window->alpha(Desktop::View::WINDOW_ALPHA_FADE)->setValueAndWarp(0.0F);
            animation.started = true;
            animation.startTime = std::chrono::steady_clock::now();
            appendDiagnostic(
                "animation started: mode=close effect=" + std::string(effect->name) +
                " class=" + animation.windowClass
            );
        }

        if (animation.closeFrame.texture == 0)
            return;

        target = GL_TEXTURE_2D;
        effectBox = animation.box;
        rounding = animation.rounding;
    } else {
        texture = surfaceTexture(window);
        if (!texture || texture->m_texID == 0)
            return;

        target = textureTarget(texture);
        effectBox = currentWindowRenderBox(window);
        rounding = window->rounding();

        if (!animation.started) {
            animation.started = true;
            animation.startTime = std::chrono::steady_clock::now();
            appendDiagnostic(
                "animation started: mode=" + std::string(animationModeName(animation.mode)) +
                " effect=" + std::string(effect->name) +
                " class=" + window->m_class
            );
        }
    }

    const bool supportsTarget =
        (target == GL_TEXTURE_2D &&
         windowEffectSupports(*effect, EWindowEffectCapability::Texture2D)) ||
        (target == GL_TEXTURE_EXTERNAL_OES &&
         windowEffectSupports(*effect, EWindowEffectCapability::ExternalTexture));
    if (target == 0 || !supportsTarget) {
        cancelAnimation("unsupported source texture type for selected effect");
        return;
    }

    if (animation.mode == EWindowAnimationMode::ManualCycle &&
        (effect->closeDurationSeconds <= 0.0F || effect->openDurationSeconds <= 0.0F)) {
        cancelAnimation("selected effect has invalid manual-cycle durations");
        return;
    }

    const float animationDuration = animationDurationSeconds(animation, *effect);
    if (animationDuration <= 0.0F) {
        cancelAnimation("selected effect has invalid automatic duration");
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::chrono::duration<float>(
        now - animation.startTime
    ).count();

    float progress = 0.0F;
    bool reverse = false;
    if (animation.mode == EWindowAnimationMode::AutomaticOpen) {
        progress = std::clamp(elapsed / animationDuration, 0.0F, 1.0F);
        reverse = true;
    } else if (animation.mode == EWindowAnimationMode::AutomaticClose) {
        progress = std::clamp(elapsed / animationDuration, 0.0F, 1.0F);
    } else if (elapsed < effect->closeDurationSeconds) {
        progress = std::clamp(
            elapsed / effect->closeDurationSeconds,
            0.0F,
            1.0F
        );
    } else {
        progress = std::clamp(
            (elapsed - effect->closeDurationSeconds) / effect->openDurationSeconds,
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

    const GLuint textureId = animation.mode == EWindowAnimationMode::AutomaticClose
        ? animation.closeFrame.texture
        : texture->m_texID;

    if (animation.mode == EWindowAnimationMode::AutomaticClose) {
        if (g_state->sceneBlitShader.program == 0) {
            cancelAnimation("close-scene blit shader is unavailable");
            return;
        }

        const auto queueSceneFrame = [&](
            const CBox& box,
            float frameRounding,
            GLuint frameTexture
        ) {
            g_pHyprRenderer->m_renderPass.add(
                makeUnique<CRealmheartEffectPassElement>(
                    CRealmheartEffectPassElement::SData{
                        .box = box,
                        .texture = frameTexture,
                        .textureTarget = GL_TEXTURE_2D,
                        .rounding = frameRounding,
                        .opacity = 1.0F,
                        .shader = &g_state->sceneBlitShader,
                    }
                )
            );
        };

        queueSceneFrame(
            animation.sceneBackdropBox,
            0.0F,
            animation.backdropFrame.texture
        );
        if (animation.freezeTiledScene) {
            for (const auto& frozen : animation.frozenSceneWindows)
                queueSceneFrame(frozen.box, frozen.rounding, frozen.frame.texture);
        }
    }

    g_pHyprRenderer->m_renderPass.add(
        makeUnique<CRealmheartEffectPassElement>(
            CRealmheartEffectPassElement::SData{
                .box = effectBox,
                .progress = progress,
                .texture = textureId,
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
            " texture=" + std::to_string(textureId) +
            " target=" + std::to_string(target) +
            " box=" + std::to_string(static_cast<int>(effectBox.width)) + "x" +
            std::to_string(static_cast<int>(effectBox.height))
        );
        if (animation.mode == EWindowAnimationMode::ManualCycle && g_handle) {
            HyprlandAPI::addNotification(
                g_handle,
                "[Realmheart FX] visible post-window pass active",
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
               window->isFullscreen()) {
        cancelAnimation("window became fullscreen during opening");
    } else if (animation.mode == EWindowAnimationMode::AutomaticOpen &&
               window->m_workspace && !window->m_workspace->isVisible()) {
        cancelAnimation("window moved to a non-visible workspace during opening");
    } else if (closing && animation.backdropCaptureFailed) {
        cancelAnimation("failed to capture the pre-window backdrop");
    } else {
        const auto now = std::chrono::steady_clock::now();
        const float armedElapsed = std::chrono::duration<float>(
            now - animation.armedTime
        ).count();

        if (!animation.started) {
            if (!window) {
                cancelAnimation(
                    closing
                        ? "closing window disappeared before snapshot capture"
                        : "window disappeared before texture capture"
                );
            } else if (armedElapsed >= kTextureWaitTimeoutSeconds) {
                cancelAnimation(
                    closing
                        ? "no usable closing backdrop and snapshot became available"
                        : "no usable window texture became available"
                );
            } else if (closing) {
                damageCloseAnimationRegion(animation);
            } else {
                g_pHyprRenderer->damageWindow(window, true);
            }
        } else {
            const float elapsed = std::chrono::duration<float>(
                now - animation.startTime
            ).count();
            const float duration = animationDurationSeconds(animation, *effect);

            if (!animation.sawPass && elapsed >= kTextureWaitTimeoutSeconds) {
                cancelAnimation("no usable window texture reached the render pass");
            } else if (elapsed >= duration && animation.terminalFrameQueued) {
                switch (animation.mode) {
                    case EWindowAnimationMode::AutomaticOpen:
                        cancelAnimation("open complete after terminal frame");
                        break;
                    case EWindowAnimationMode::AutomaticClose:
                        restartFrozenSceneReflow(animation);
                        cancelAnimation("close complete; native tiled reflow restarted");
                        break;
                    case EWindowAnimationMode::ManualCycle:
                        cancelAnimation("cycle complete after terminal frame");
                        break;
                }
            } else if (closing) {
                damageCloseAnimationRegion(animation);
            } else {
                damageExpandedBox(currentWindowRenderBox(window));
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
        cancelAnimation("target window began closing");

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
        g_state->effectRandom()
    );
    if (effectName == kNoWindowEffect) {
        appendDiagnostic(
            "automatic close skipped: class=" + std::string(windowClass) +
            " liveClass=" + window->m_class +
            " initialClass=" + window->m_initialClass +
            " reason=assignment resolved to none or class is excluded"
        );
        return;
    }

    if (g_state->animation.active) {
        appendDiagnostic(
            "automatic close skipped: class=" + std::string(windowClass) +
            " reason=another animation is active"
        );
        return;
    }

    const SWindowEffectSpec* effect = findWindowEffect(effectName);
    if (effect == nullptr) {
        appendDiagnostic(
            "automatic close skipped: class=" + std::string(windowClass) +
            " reason=assigned effect is missing"
        );
        return;
    }

    std::string reason;
    if (!automaticCloseEligibility(window, *effect, reason)) {
        appendDiagnostic(
            "automatic close skipped: class=" + std::string(windowClass) +
            " reason=" + reason
        );
        return;
    }

    const auto monitor = window->m_monitor.lock();
    const bool freezeTiledScene = !window->m_isFloating;
    std::vector<SFrozenSceneWindow> frozenSceneWindows;
    if (freezeTiledScene &&
        !captureFrozenSceneWindows(window, monitor, frozenSceneWindows, reason)) {
        appendDiagnostic(
            "automatic close skipped: class=" + std::string(windowClass) +
            " reason=pre-reflow scene capture failed: " + reason
        );
        return;
    }

    const CBox closingBox = currentWindowRenderBox(window);
    const CBox sceneBackdropBox =
        closeSceneBackdropBox(closingBox, frozenSceneWindows);
    const auto now = std::chrono::steady_clock::now();
    g_state->animation = {
        .window = window,
        .monitor = monitor,
        .effect = effect,
        .mode = EWindowAnimationMode::AutomaticClose,
        .box = closingBox,
        .rounding = window->rounding(),
        .windowClass = std::string(windowClass),
        .armedTime = now,
        .startTime = {},
        .closeFrame = {},
        .backdropFrame = {},
        .sceneBackdropBox = sceneBackdropBox,
        .frozenSceneWindows = std::move(frozenSceneWindows),
        .freezeTiledScene = freezeTiledScene,
        .active = true,
        .started = false,
        .sawPass = false,
        .terminalFrameQueued = false,
        .backdropCaptureQueued = false,
        .backdropCaptureFailed = false,
    };

    appendDiagnostic(
        "automatic close armed: effect=" + std::string(effect->name) +
        " class=" + std::string(windowClass) + " title=" + window->m_title +
        " frozenWindows=" +
        std::to_string(g_state->animation.frozenSceneWindows.size()) +
        " sceneBackdrop=" +
        std::to_string(static_cast<int>(sceneBackdropBox.width)) + "x" +
        std::to_string(static_cast<int>(sceneBackdropBox.height))
    );
    damageCloseAnimationRegion(g_state->animation);
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
            appendDiagnostic("automatic close enabled");
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

} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    {
        std::ofstream stream(kDiagnosticLog, std::ios::trunc);
        if (stream)
            stream << "Realmheart FX render-pass pluginInit\n";
    }

    const auto runtime = HyprlandAPI::getHyprlandVersion(handle);
    if (runtime.hash != GIT_COMMIT_HASH) {
        throw std::runtime_error(
            "Realmheart FX was built for Hyprland " + std::string(GIT_COMMIT_HASH) +
            " but the running compositor is " + runtime.hash
        );
    }

    const auto registry = loadWindowEffectRegistry(REALMHEART_EFFECT_ASSET_DIR);
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

    g_state->tick = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, &onTick, nullptr);
    if (!g_state->tick)
        throw std::runtime_error("failed to create Realmheart FX frame timer");
    wl_event_source_timer_update(g_state->tick, 1);

    appendDiagnostic("automatic open policy enabled");
    appendDiagnostic("automatic close policy enabled");

    return {
        .name = "Realmheart FX",
        .description = "Realmheart transitions through Hyprland's visible render pass",
        .author = "Zahed; render-pass plumbing adapted from  hyprfx/xhos hyprfx",
        .version = "0.9.1-random-effect-pools",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listeners.clear();

    if (g_state) {
        cancelAnimation("plugin unload");

        if (g_state->command)
            HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_state->command);
        if (g_state->tick)
            wl_event_source_remove(g_state->tick);

        g_pHyprRenderer->m_renderPass.removeAllOfType("CRealmheartBackdropCapturePassElement");
        g_pHyprRenderer->m_renderPass.removeAllOfType("CRealmheartEffectPassElement");
        g_pHyprOpenGL->makeEGLCurrent();
        for (auto& effect : g_state->effects)
            effect.destroy();
        g_state->effects.clear();
        g_state->sceneBlitShader.destroy();
        g_state.reset();
    }

    g_handle = nullptr;
}
