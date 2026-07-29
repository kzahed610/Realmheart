// SPDX-License-Identifier: GPL-3.0-or-later
// Compositor render-pass integration adapted from  hyprfx (commit
// d680dabdd2d9362626ecedcad9bd396508163468), itself derived from xhos/hyprfx.

#define WLR_USE_UNSTABLE

#include "RealmheartVoidPassElement.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleApplicator.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/core/Subcompositor.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/version.h>
#include <hyprland/src/xwayland/XSurface.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Render::GL;

namespace {

constexpr float kHalfDurationSeconds = 0.85F;
constexpr float kCycleDurationSeconds = kHalfDurationSeconds * 2.0F;
constexpr float kTextureWaitTimeoutSeconds = 0.60F;
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

struct STestAnimation {
    PHLWINDOWREF window;
    std::chrono::steady_clock::time_point startTime{};
    bool active = false;
    bool sawPass = false;
};

struct SPluginState {
    SRealmheartVoidShader shaders[2];
    STestAnimation test;
    wl_event_source* tick = nullptr;
    SP<SHyprCtlCommand> command;
};

HANDLE g_handle = nullptr;
UP<SPluginState> g_state;
std::vector<Hyprutils::Signal::CHyprSignalListener> g_listeners;

void appendDiagnostic(const std::string& message) {
    std::ofstream stream(kDiagnosticLog, std::ios::app);
    if (stream)
        stream << message << '\n';
}

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

std::string readVoidShader() {
    std::ifstream stream(REALMHEART_VOID_SHADER_FILE, std::ios::binary);
    if (!stream)
        throw std::runtime_error(
            "could not read Realmheart Void shader at " REALMHEART_VOID_SHADER_FILE
        );

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

void fillLocations(SRealmheartVoidShader& shader) {
    shader.locations.at(static_cast<std::size_t>(ERealmheartVoidUniform::Projection)) =
        glGetUniformLocation(shader.program, "proj");
    shader.locations.at(static_cast<std::size_t>(ERealmheartVoidUniform::Position)) =
        glGetAttribLocation(shader.program, "pos");
    shader.locations.at(static_cast<std::size_t>(ERealmheartVoidUniform::Progress)) =
        glGetUniformLocation(shader.program, "progress");
    shader.locations.at(static_cast<std::size_t>(ERealmheartVoidUniform::Resolution)) =
        glGetUniformLocation(shader.program, "resolution");
    shader.locations.at(static_cast<std::size_t>(ERealmheartVoidUniform::Texture)) =
        glGetUniformLocation(shader.program, "tex");
    shader.locations.at(static_cast<std::size_t>(ERealmheartVoidUniform::Radius)) =
        glGetUniformLocation(shader.program, "radius");
    shader.locations.at(static_cast<std::size_t>(ERealmheartVoidUniform::Reverse)) =
        glGetUniformLocation(shader.program, "reverse");
    shader.locations.at(static_cast<std::size_t>(ERealmheartVoidUniform::Gold)) =
        glGetUniformLocation(shader.program, "uGold");
    shader.locations.at(static_cast<std::size_t>(ERealmheartVoidUniform::Starlight)) =
        glGetUniformLocation(shader.program, "uStarlight");
    shader.locations.at(static_cast<std::size_t>(ERealmheartVoidUniform::Astral)) =
        glGetUniformLocation(shader.program, "uAstral");
    shader.locations.at(static_cast<std::size_t>(ERealmheartVoidUniform::Void)) =
        glGetUniformLocation(shader.program, "uVoid");
}

void initialiseShaders() {
    g_pHyprOpenGL->makeEGLCurrent();

    const std::string fragment = readVoidShader();
    g_state->shaders[0].program = createProgram(kVertexShader, fragment);
    fillLocations(g_state->shaders[0]);

    g_state->shaders[1].program = createProgram(kVertexShader, externalVariant(fragment));
    fillLocations(g_state->shaders[1]);
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

    g_pHyprRenderer->damageBox(CBox{
        window->m_position.x,
        window->m_position.y,
        window->m_size.x,
        window->m_size.y,
    });
}

void cancelTest(const std::string& reason) {
    if (!g_state || !g_state->test.active)
        return;

    const auto window = g_state->test.window.lock();
    clearWindowHidden(window);
    g_state->test = {};
    g_pHyprRenderer->m_renderPass.removeAllOfType("CRealmheartVoidPassElement");
    appendDiagnostic("test ended: " + reason);
}

std::string armFocusedWindow() {
    const PHLWINDOW window = Desktop::focusState()->window();
    if (!window || !window->m_isMapped)
        return "focus a mapped application window first";
    if (window->isFullscreen())
        return "fullscreen windows are excluded";
    if (window->isX11OverrideRedirect())
        return "override-redirect windows are excluded";
    if (!window->m_ruleApplicator)
        return "focused window has no rule applicator";

    if (g_state->test.active)
        cancelTest("restarted");

    setWindowHidden(window);
    g_state->test = {
        .window = window,
        .startTime = std::chrono::steady_clock::now(),
        .active = true,
        .sawPass = false,
    };

    appendDiagnostic(
        "armed render-pass test: class=" + window->m_class +
        " title=" + window->m_title
    );

    if (g_handle) {
        HyprlandAPI::addNotification(
            g_handle,
            "[Realmheart FX] render-pass test armed on " + window->m_class,
            CHyprColor{0.55F, 0.38F, 0.95F, 1.0F},
            3000.0F
        );
    }

    g_pHyprRenderer->damageWindow(window, true);
    return "ok";
}

void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_POST_WINDOWS || !g_state || !g_state->test.active)
        return;

    const auto window = g_state->test.window.lock();
    if (!window || !window->m_isMapped) {
        cancelTest("window disappeared");
        return;
    }

    const auto currentMonitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
    const auto windowMonitor = window->m_monitor.lock();
    if (windowMonitor && currentMonitor && windowMonitor != currentMonitor)
        return;

    const auto texture = surfaceTexture(window);
    if (!texture || texture->m_texID == 0)
        return;

    const GLenum target = textureTarget(texture);
    if (target == 0) {
        cancelTest("unsupported source texture type");
        return;
    }

    const float elapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - g_state->test.startTime
    ).count();

    float progress = 0.0F;
    bool reverse = false;
    if (elapsed < kHalfDurationSeconds) {
        progress = std::clamp(elapsed / kHalfDurationSeconds, 0.0F, 1.0F);
    } else {
        progress = std::clamp(
            (elapsed - kHalfDurationSeconds) / kHalfDurationSeconds,
            0.0F,
            1.0F
        );
        reverse = true;
    }

    const auto& shader = g_state->shaders[target == GL_TEXTURE_EXTERNAL_OES ? 1 : 0];
    g_pHyprRenderer->m_renderPass.add(
        makeUnique<CRealmheartVoidPassElement>(
            CRealmheartVoidPassElement::SData{
                .box = CBox{
                    window->m_position.x,
                    window->m_position.y,
                    window->m_size.x,
                    window->m_size.y,
                },
                .progress = progress,
                .texture = texture->m_texID,
                .textureTarget = target,
                .rounding = window->rounding(),
                .reverse = reverse,
                .shader = &shader,
            }
        )
    );

    if (!g_state->test.sawPass) {
        g_state->test.sawPass = true;
        appendDiagnostic(
            "visible render pass queued: texture=" + std::to_string(texture->m_texID) +
            " target=" + std::to_string(target) +
            " box=" + std::to_string(static_cast<int>(window->m_size.x)) + "x" +
            std::to_string(static_cast<int>(window->m_size.y))
        );
        if (g_handle) {
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

    if (!g_state->test.active) {
        wl_event_source_timer_update(g_state->tick, 250);
        return 0;
    }

    const auto window = g_state->test.window.lock();
    if (!window || !window->m_isMapped) {
        cancelTest("window disappeared during tick");
    } else {
        const float elapsed = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - g_state->test.startTime
        ).count();

        if (!g_state->test.sawPass && elapsed >= kTextureWaitTimeoutSeconds) {
            cancelTest("no usable window texture reached the render pass");
        } else if (elapsed >= kCycleDurationSeconds) {
            cancelTest("cycle complete");
        } else {
            g_pHyprRenderer->damageBox(CBox{
                window->m_position.x - 2,
                window->m_position.y - 2,
                window->m_size.x + 4,
                window->m_size.y + 4,
            });
        }
    }

    const int timeout = g_pHyprRenderer->m_mostHzMonitor
        ? static_cast<int>(1000.0 / g_pHyprRenderer->m_mostHzMonitor->m_refreshRate)
        : 16;
    wl_event_source_timer_update(g_state->tick, std::max(timeout, 1));
    return 0;
}

void onWindowClose(PHLWINDOW window) {
    if (!g_state || !g_state->test.active)
        return;
    if (g_state->test.window.lock() == window)
        cancelTest("target window closed");
}

void onWorkspace(PHLWORKSPACE) {
    if (g_state && g_state->test.active)
        cancelTest("workspace changed");
}

std::string controlCommand(eHyprCtlOutputFormat format, std::string request) {
    (void)format;

    std::istringstream stream(std::move(request));
    std::string command;
    std::string subcommand;
    stream >> command >> subcommand;

    if (subcommand == "test")
        return armFocusedWindow();
    if (subcommand == "status")
        return g_state && g_state->test.active ? "active" : "idle";
    if (subcommand == "cancel") {
        cancelTest("cancelled by user");
        return "ok";
    }

    return "usage: realmheart-fx test|status|cancel";
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

    g_state = makeUnique<SPluginState>();
    initialiseShaders();

    auto& events = Event::bus()->m_events;
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

    const std::string result = armFocusedWindow();
    if (result != "ok") {
        HyprlandAPI::addNotification(
            handle,
            "[Realmheart FX] " + result,
            CHyprColor{1.0F, 0.32F, 0.62F, 1.0F},
            6000.0F
        );
    }

    return {
        .name = "Realmheart FX",
        .description = "Realmheart transitions through Hyprland's visible render pass",
        .author = "Zahed; render-pass plumbing adapted from  hyprfx/xhos hyprfx",
        .version = "0.2.3--void-base",
    };
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listeners.clear();

    if (g_state) {
        cancelTest("plugin unload");

        if (g_state->command)
            HyprlandAPI::unregisterHyprCtlCommand(g_handle, g_state->command);
        if (g_state->tick)
            wl_event_source_remove(g_state->tick);

        g_pHyprRenderer->m_renderPass.removeAllOfType("CRealmheartVoidPassElement");
        g_pHyprOpenGL->makeEGLCurrent();
        g_state->shaders[0].destroy();
        g_state->shaders[1].destroy();
        g_state.reset();
    }

    g_handle = nullptr;
}
