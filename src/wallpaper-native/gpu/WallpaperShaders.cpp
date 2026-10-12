#include "wallpaper-native/gpu/WallpaperShaders.hpp"

#include "wallpaper-native/gpu/YuvColorimetry.hpp"

#include <array>
#include <string_view>

namespace realmheart::wallpaper_native {

namespace {

constexpr char kVertexShader[] = R"(
attribute vec2 a_position;
attribute vec2 a_uv;
varying vec2 v_uv;
void main() {
    v_uv = a_uv;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)";

// One program, every combination.
//
// Each source binds either an RGB texture or a YUV luma + chroma pair, and the
// fragment shader picks with a flag. That keeps RGB->RGB, RGB->YUV, YUV->RGB and
// YUV->YUV transitions in a single program instead of four variants that have
// to stay in sync (renderer docs section 12). The conversion happens per
// sampled pixel on the GPU, so it never costs a CPU pass and never costs the
// extra full-frame normalise pass of the alternative.
constexpr char kFragmentShader[] = R"(
precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_current;
uniform sampler2D u_next;
uniform sampler2D u_current_chroma;
uniform sampler2D u_next_chroma;
uniform int u_current_is_yuv;
uniform int u_next_is_yuv;
uniform float u_progress;
uniform vec4 u_current_uv;
uniform vec4 u_next_uv;
uniform mat3 u_current_yuv;
uniform mat3 u_next_yuv;
uniform float u_current_yuv_offset;
uniform float u_next_yuv_offset;

vec2 crop_uv(vec4 rect, vec2 base_uv) {
    return mix(rect.xy, rect.zw, base_uv);
}

// Chroma arrives as a LUMINANCE_ALPHA pair through .ra, so no de-interleaving
// pass ever runs on the CPU.
vec3 fetch_yuv(sampler2D luma, sampler2D chroma, vec2 uv, mat3 matrix, float offset) {
    float y = texture2D(luma, uv).r + offset;
    vec2 pair = texture2D(chroma, uv).ra - vec2(0.5);
    return clamp(matrix * vec3(y, pair.x, pair.y), 0.0, 1.0);
}

vec3 fetch_rgb(sampler2D plane, vec2 uv) {
    return texture2D(plane, uv).rgb;
}

void main() {
    vec2 current_uv = crop_uv(u_current_uv, v_uv);
    vec3 from_color = u_current_is_yuv == 1
        ? fetch_yuv(u_current, u_current_chroma, current_uv,
                    u_current_yuv, u_current_yuv_offset)
        : fetch_rgb(u_current, current_uv);
    vec3 to_color;
    if (u_progress <= 0.0) {
        to_color = from_color;
    } else {
        vec2 next_uv = crop_uv(u_next_uv, v_uv);
        to_color = u_next_is_yuv == 1
            ? fetch_yuv(u_next, u_next_chroma, next_uv,
                        u_next_yuv, u_next_yuv_offset)
            : fetch_rgb(u_next, next_uv);
    }
    gl_FragColor = vec4(mix(from_color, to_color, clamp(u_progress, 0.0, 1.0)), 1.0);
}
)";

} // namespace

WallpaperShaders::~WallpaperShaders() { release(); }

void WallpaperShaders::release() noexcept {
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

bool WallpaperShaders::initialize(std::string* error_message) {
    release();
    if (error_message != nullptr) error_message->clear();

    const auto compile = [&](GLenum type, const char* source, GLuint& shader) {
        shader = glCreateShader(type);
        if (shader == 0) return false;
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint status = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        return status == GL_TRUE;
    };

    GLuint vertex = 0;
    GLuint fragment = 0;
    if (!compile(GL_VERTEX_SHADER, kVertexShader, vertex) ||
        !compile(GL_FRAGMENT_SHADER, kFragmentShader, fragment)) {
        if (error_message != nullptr) {
            GLint length = 0;
            glGetShaderiv(fragment != 0 ? fragment : vertex,
                         GL_INFO_LOG_LENGTH,
                         &length);
            std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
            glGetShaderInfoLog(
                fragment != 0 ? fragment : vertex, length, nullptr, log.data()
            );
            *error_message = "wallpaper shader compilation failed: " + log;
        }
        if (fragment != 0) glDeleteShader(fragment);
        if (vertex != 0) glDeleteShader(vertex);
        return false;
    }

    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint link_status = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &link_status);
    if (link_status != GL_TRUE) {
        if (error_message != nullptr) {
            GLint length = 0;
            glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<std::size_t>(std::max(length, 1)), '\0');
            glGetProgramInfoLog(program_, length, nullptr, log.data());
            *error_message = "wallpaper shader linking failed: " + log;
        }
        glDeleteProgram(program_);
        program_ = 0;
        return false;
    }

    position_attribute_ = glGetAttribLocation(program_, "a_position");
    uv_attribute_ = glGetAttribLocation(program_, "a_uv");
    current_sampler_ = glGetUniformLocation(program_, "u_current");
    next_sampler_ = glGetUniformLocation(program_, "u_next");
    current_chroma_sampler_ = glGetUniformLocation(program_, "u_current_chroma");
    next_chroma_sampler_ = glGetUniformLocation(program_, "u_next_chroma");
    current_is_yuv_ = glGetUniformLocation(program_, "u_current_is_yuv");
    next_is_yuv_ = glGetUniformLocation(program_, "u_next_is_yuv");
    progress_uniform_ = glGetUniformLocation(program_, "u_progress");
    current_crop_uniform_ = glGetUniformLocation(program_, "u_current_uv");
    next_crop_uniform_ = glGetUniformLocation(program_, "u_next_uv");
    current_yuv_matrix_ = glGetUniformLocation(program_, "u_current_yuv");
    next_yuv_matrix_ = glGetUniformLocation(program_, "u_next_yuv");
    current_yuv_offset_ = glGetUniformLocation(program_, "u_current_yuv_offset");
    next_yuv_offset_ = glGetUniformLocation(program_, "u_next_yuv_offset");
    return true;
}

void WallpaperShaders::draw(
    const RenderFrameView& current,
    const RenderFrameView& incoming,
    std::array<float, 4> current_crop,
    std::array<float, 4> incoming_crop,
    float progress
) const noexcept {
    if (program_ == 0 || !current.valid()) return;

    const bool has_incoming = incoming.valid();
    // With no incoming source the next sampler reads the *current* frame, and it
    // has to be told what kind of frame that is: binding a luma texture as RGB
    // makes a live wallpaper render as flat grey.
    const RenderFrameView& next = has_incoming ? incoming : current;

    // An RGB source gets an identity matrix, which the is-yuv flag makes the
    // shader ignore.
    const auto yuv_of = [](const RenderFrameView& frame) {
        return yuv_conversion_for(frame.color_space(), frame.color_range());
    };
    const YuvConversion current_yuv = yuv_of(current);
    const YuvConversion incoming_yuv = yuv_of(next);

    // GL wants matrices column-major; YuvConversion is row-major.
    const auto column_major = [](const YuvConversion& conversion) {
        return std::array<float, 9>{
            conversion.matrix[0], conversion.matrix[3], conversion.matrix[6],
            conversion.matrix[1], conversion.matrix[4], conversion.matrix[7],
            conversion.matrix[2], conversion.matrix[5], conversion.matrix[8]
        };
    };
    const std::array<float, 9> current_columns = column_major(current_yuv);
    const std::array<float, 9> incoming_columns = column_major(incoming_yuv);

    constexpr std::array<float, 8> positions{
        -1.0F, -1.0F,
         1.0F, -1.0F,
        -1.0F,  1.0F,
         1.0F,  1.0F,
    };
    constexpr std::array<float, 8> uvs{
        0.0F, 0.0F,
        1.0F, 0.0F,
        0.0F, 1.0F,
        1.0F, 1.0F,
    };

    glUseProgram(program_);
    glEnableVertexAttribArray(static_cast<GLuint>(position_attribute_));
    glEnableVertexAttribArray(static_cast<GLuint>(uv_attribute_));
    glVertexAttribPointer(
        static_cast<GLuint>(position_attribute_), 2, GL_FLOAT, GL_FALSE, 0,
        positions.data()
    );
    glVertexAttribPointer(
        static_cast<GLuint>(uv_attribute_), 2, GL_FLOAT, GL_FALSE, 0, uvs.data()
    );

    glUniform1i(current_sampler_, 0);
    glUniform1i(next_sampler_, 1);
    glUniform1i(current_chroma_sampler_, 2);
    glUniform1i(next_chroma_sampler_, 3);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, current.texture());
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, next.texture());
    // Every sampler unit needs a bound texture; the RGB one is as good as any
    // and is never sampled, because the is-yuv flag gates the chroma path.
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, current.chroma_texture());
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, next.chroma_texture());

    glUniform1i(
        current_is_yuv_, current.kind() == RenderFrameKind::Nv12Planes ? 1 : 0
    );
    glUniform1i(
        next_is_yuv_, next.kind() == RenderFrameKind::Nv12Planes ? 1 : 0
    );
    glUniformMatrix3fv(current_yuv_matrix_, 1, GL_FALSE, current_columns.data());
    glUniformMatrix3fv(next_yuv_matrix_, 1, GL_FALSE, incoming_columns.data());
    glUniform1f(current_yuv_offset_, current_yuv.luma_offset);
    glUniform1f(next_yuv_offset_, incoming_yuv.luma_offset);

    glUniform1f(progress_uniform_, has_incoming ? progress : 1.0F);
    glUniform4fv(current_crop_uniform_, 1, current_crop.data());
    glUniform4fv(next_crop_uniform_, 1, incoming_crop.data());

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(static_cast<GLuint>(position_attribute_));
    glDisableVertexAttribArray(static_cast<GLuint>(uv_attribute_));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace realmheart::wallpaper_native
