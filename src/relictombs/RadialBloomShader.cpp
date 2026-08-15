#include "relictombs/RadialBloomShader.hpp"

#include <fstream>
#include <iostream>
#include <array>
#include <optional>
#include <epoxy/gl.h>
#include <epoxy/glx.h>

namespace realmheart::relictombs {

RadialBloomShader::RadialBloomShader() = default;

RadialBloomShader::~RadialBloomShader() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (program_) glDeleteProgram(program_);
}

bool RadialBloomShader::compile() {
    auto source = read_shader_file();
    if (source.empty()) {
        std::cerr << "[RadialBloomShader] Failed to read shader file\n";
        return false;
    }

    GLuint vs = compile_shader(GL_VERTEX_SHADER, source);
    if (!vs) return false;

    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, source);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint log_len = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &log_len);
        std::string log(log_len, '\0');
        glGetProgramInfoLog(program_, log_len, nullptr, log.data());
        std::cerr << "[RadialBloomShader] Link failed: " << log << '\n';
        glDeleteProgram(program_);
        program_ = 0;
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    // Get uniform locations
    u_origin_ = glGetUniformLocation(program_, "u_origin");
    u_radius_ = glGetUniformLocation(program_, "u_radius");
    u_wallpaper_ = glGetUniformLocation(program_, "u_wallpaper");
    u_alpha_ = glGetUniformLocation(program_, "u_alpha");
    a_position_ = glGetAttribLocation(program_, "a_position");
    a_texcoord_ = glGetAttribLocation(program_, "a_texcoord");

    // Full-screen quad VAO
    static constexpr std::array<float, 16> quad_data = {
        // position      // texcoord
        -1.0f, -1.0f,   0.0f, 0.0f,
         1.0f, -1.0f,   1.0f, 0.0f,
        -1.0f,  1.0f,   0.0f, 1.0f,
         1.0f,  1.0f,   1.0f, 1.0f,
    };

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, quad_data.size() * sizeof(float), quad_data.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(a_position_);
    glVertexAttribPointer(a_position_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    glEnableVertexAttribArray(a_texcoord_);
    glVertexAttribPointer(a_texcoord_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void RadialBloomShader::set_origin(float x, float y) {
    if (program_) {
        glUseProgram(program_);
        glUniform2f(u_origin_, x, y);
    }
}

void RadialBloomShader::set_radius(float radius) {
    if (program_) {
        glUseProgram(program_);
        glUniform1f(u_radius_, radius);
    }
}

void RadialBloomShader::set_alpha(float alpha) {
    if (program_) {
        glUseProgram(program_);
        glUniform1f(u_alpha_, alpha);
    }
}

void RadialBloomShader::draw(GLuint wallpaper_texture, int width, int height) {
    if (!program_ || !wallpaper_texture) return;

    glUseProgram(program_);
    glBindVertexArray(vao_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, wallpaper_texture);
    glUniform1i(u_wallpaper_, 0);

    glViewport(0, 0, width, height);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void RadialBloomShader::use() const {
    if (program_) glUseProgram(program_);
}

void RadialBloomShader::unuse() const {
    glUseProgram(0);
}

bool RadialBloomShader::load_shader_source(const std::string& path, std::string& out_source) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    out_source.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

GLuint RadialBloomShader::compile_shader(GLenum type, std::string source) {
    // Extract vertex or fragment part based on markers
    std::string shader_source;
    if (type == GL_VERTEX_SHADER) {
        shader_source = R"(
#version 330 core
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_texcoord;
out vec2 v_uv;
void main() {
    v_uv = a_texcoord;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";
    } else {
        shader_source = source;
    }

    GLuint shader = glCreateShader(type);
    const char* src = shader_source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint log_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        std::string log(log_len, '\0');
        glGetShaderInfoLog(shader, log_len, nullptr, log.data());
        std::cerr << "[RadialBloomShader] " << (type == GL_VERTEX_SHADER ? "Vertex" : "Fragment")
                  << " shader compile failed: " << log << '\n';
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

std::string RadialBloomShader::read_shader_file() {
    std::string source;
    // Try multiple possible locations
    static constexpr const char* paths[] = {
        "assets/shaders/radial_bloom.frag",
        "../assets/shaders/radial_bloom.frag",
        "../../assets/shaders/radial_bloom.frag",
        "../../../assets/shaders/radial_bloom.frag",
    };

    for (const char* path : paths) {
        if (load_shader_source(path, source)) {
            return source;
        }
    }

    std::cerr << "[RadialBloomShader] Could not find radial_bloom.frag in any expected location\n";
    return "";
}

} // namespace realmheart::relictombs