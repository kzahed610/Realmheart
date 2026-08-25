#include "ui/lockscreen/ShaderManager.hpp"

#include "effects/core/ShaderSource.hpp"

#include <epoxy/gl.h>
#include <glib.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace realmheart::ui::lockscreen {
namespace {

constexpr std::string_view kScalesShaderAsset = "lockscreen/scales/scales.frag";
constexpr int kShaderReloadIntervalMs = 250;

// Fullscreen triangle vertex shader shared by all lockscreen fragments. It
// derives the clip-space corner from gl_VertexID, so no VBO is needed.
constexpr std::string_view kVertexShader = R"GLSL(#version 300 es
precision highp float;

out vec2 v_texcoord;

void main() {
    vec2 corner = vec2(
        float((gl_VertexID << 1) & 2),
        float(gl_VertexID & 2)
    );
    v_texcoord = vec2(corner.x, 1.0 - corner.y);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

GLuint compile_shader(
    GLenum type,
    std::string_view source,
    std::string* error
) {
    const GLuint shader = glCreateShader(type);
    const char* source_pointer = source.data();
    const GLint source_length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &source_pointer, &source_length);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE) return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    glDeleteShader(shader);
    set_error(error, std::move(log));
    return 0;
}

GLuint link_program(
    std::string_view fragment_source,
    std::string* error
) {
    std::string vertex_error;
    const GLuint vertex = compile_shader(
        GL_VERTEX_SHADER,
        kVertexShader,
        &vertex_error
    );
    if (vertex == 0) {
        set_error(error, "vertex shader compilation failed: " + vertex_error);
        return 0;
    }

    std::string fragment_error;
    const GLuint fragment = compile_shader(
        GL_FRAGMENT_SHADER,
        fragment_source,
        &fragment_error
    );
    if (fragment == 0) {
        glDeleteShader(vertex);
        set_error(error, "fragment shader compilation failed: " + fragment_error);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE) return program;

    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    glDeleteProgram(program);
    set_error(error, "shader link failed: " + log);
    return 0;
}

} // namespace

// A single shader program plus its hot-reload bookkeeping.
struct ShaderManager::Program {
    GLuint program = 0;
    std::string source;
    std::string path;
    std::filesystem::file_time_type last_write_time{};
};

struct ShaderManager::State {
    Program scales;
    guint reload_source_id = 0;
    bool watching = false;

    void stop_watching() noexcept {
        if (reload_source_id != 0) {
            g_source_remove(reload_source_id);
            reload_source_id = 0;
        }
        watching = false;
    }

    void delete_programs() noexcept {
        if (scales.program != 0) {
            glDeleteProgram(scales.program);
            scales.program = 0;
        }
    }
};

ShaderManager::ShaderManager() : state_(new State) {}

ShaderManager::~ShaderManager() {
    if (state_ == nullptr) return;
    state_->stop_watching();
    state_->delete_programs();
    delete state_;
    state_ = nullptr;
}

// Reloads the program if its source file changed since the last check.
void ShaderManager::reload_program_if_changed(Program& program) {
    if (program.path.empty()) return;

    std::error_code error;
    const auto modified = std::filesystem::last_write_time(program.path, error);
    if (error || modified == program.last_write_time) return;
    program.last_write_time = modified;

    const auto shader = realmheart::effects::load_shader_source(program.path);
    if (!shader) return;

    std::string missing_symbol;
    if (!realmheart::effects::validate_lockscreen_shader_contract(
            shader->text,
            &missing_symbol)) {
        std::cerr << "[BrokenSeal] hot-reload rejected: missing "
                  << missing_symbol << '\n';
        return;
    }

    std::string link_error;
    const GLuint linked = link_program(shader->text, &link_error);
    if (linked == 0) {
        std::cerr << "[BrokenSeal] hot-reload compile failed: "
                  << link_error << '\n';
        return;
    }

    if (program.program != 0) glDeleteProgram(program.program);
    program.program = linked;
    program.source = shader->text;
    std::cerr << "[BrokenSeal] hot-reloaded " << program.path << '\n';
}

gboolean ShaderManager::reload_tick(gpointer data) {
    auto* state = static_cast<ShaderManager::State*>(data);
    if (!state->watching) return G_SOURCE_REMOVE;
    reload_program_if_changed(state->scales);
    return G_SOURCE_CONTINUE;
}

bool ShaderManager::ensure_loaded(std::string* error) {
    if (state_ == nullptr) {
        set_error(error, "shader manager is unavailable");
        return false;
    }
    if (state_->scales.program != 0) return true;

    const auto shader = realmheart::effects::load_shader_source(
        kScalesShaderAsset,
        error
    );
    if (!shader) return false;

    std::string missing_symbol;
    if (!realmheart::effects::validate_lockscreen_shader_contract(
            shader->text,
            &missing_symbol)) {
        set_error(error, "scales shader contract is missing: " + missing_symbol);
        return false;
    }

    const GLuint program = link_program(shader->text, error);
    if (program == 0) return false;

    state_->scales.program = program;
    state_->scales.source = shader->text;
    state_->scales.path = shader->path;
    std::error_code stat_error;
    state_->scales.last_write_time = std::filesystem::last_write_time(
        shader->path,
        stat_error
    );

    if (!state_->watching) {
        state_->watching = true;
        state_->reload_source_id = g_timeout_add(
            kShaderReloadIntervalMs,
            reload_tick,
            state_
        );
    }
    return true;
}

unsigned int ShaderManager::program() const noexcept {
    return state_ != nullptr ? state_->scales.program : 0;
}

} // namespace realmheart::ui::lockscreen
