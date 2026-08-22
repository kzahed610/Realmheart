// horn-preview: offscreen EGL/GLES3 renderer for the Broken Seal shader.
// Renders the PRODUCTION fragment shader asset to PNG-sequence PPM frames
// so iterations can be judged locally without touching the live shell.
//
// Usage: horn-preview <crystal.frag> [W] [H] [/tmp/prefix] [frames_csv]
//   frames_csv: comma-separated progress values rendered as frames,
//               default "0.25,0.5,0.75,1.0"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_file(const char* path) {
    std::ifstream file(path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

GLuint compile_shader(GLenum type, const std::string& source) {
    GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader compile failed:\n%s\n", log);
        std::exit(1);
    }
    return shader;
}

void write_ppm(const std::string& path, int w, int h,
               const std::vector<unsigned char>& pixels) {
    // GL reads bottom-up; PPM wants top-down, so flip rows.
    std::vector<unsigned char> flipped(pixels.size());
    const size_t row = static_cast<size_t>(w) * 3;
    for (int y = 0; y < h; ++y) {
        std::memcpy(&flipped[row * y], &pixels[row * (h - 1 - y)], row);
    }
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) { std::perror("fopen"); std::exit(1); }
    std::fprintf(out, "P6\n%d %d\n255\n", w, h);
    std::fwrite(flipped.data(), 1, flipped.size(), out);
    std::fclose(out);
}

} // namespace

int main(int argc, char** argv) {
    const char* frag_path = argc > 1 ? argv[1]
        : "effects/lockscreen/crystal/crystal.frag";
    const int width = argc > 2 ? std::atoi(argv[2]) : 960;
    const int height = argc > 3 ? std::atoi(argv[3]) : 600;
    const std::string prefix = argc > 4 ? argv[4] : "/tmp/hornprev/frame";
    std::vector<float> progresses;
    if (argc > 5) {
        std::string csv = argv[5];
        std::stringstream stream(csv);
        std::string item;
        while (std::getline(stream, item, ',')) {
            progresses.push_back(std::strtof(item.c_str(), nullptr));
        }
    } else {
        progresses = {0.25f, 0.5f, 0.75f, 1.0f};
    }

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        std::fprintf(stderr, "eglGetDisplay failed\n");
        return 1;
    }
    if (!eglInitialize(display, nullptr, nullptr)) {
        std::fprintf(stderr, "eglInitialize failed: 0x%x\n", eglGetError());
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, config_attrs, &config, 1, &count) || count < 1) {
        std::fprintf(stderr, "eglChooseConfig failed\n");
        return 1;
    }
    const EGLint pbuffer_attrs[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbuffer_attrs);
    if (surface == EGL_NO_SURFACE) {
        std::fprintf(stderr, "eglCreatePbufferSurface failed: 0x%x\n", eglGetError());
        return 1;
    }
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(display, surface, surface, context)) {
        std::fprintf(stderr, "context creation failed: 0x%x\n", eglGetError());
        return 1;
    }

    const std::string vertex_source =
        "#version 300 es\n"
        "precision highp float;\n"
        "out vec2 v_texcoord;\n"
        "void main() {\n"
        "    vec2 corner = vec2(\n"
        "        float((gl_VertexID << 1) & 2),\n"
        "        float(gl_VertexID & 2)\n"
        "    );\n"
        "    v_texcoord = vec2(corner.x, 1.0 - corner.y);\n"
        "    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";

    GLuint program = glCreateProgram();
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, read_file(frag_path));
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[4096];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "link failed:\n%s\n", log);
        return 1;
    }
    glUseProgram(program);
    glViewport(0, 0, width, height);

    for (size_t i = 0; i < progresses.size(); ++i) {
        const float t = progresses[i];
        glUniform2f(glGetUniformLocation(program, "resolution"),
                    static_cast<float>(width), static_cast<float>(height));
        glUniform1f(glGetUniformLocation(program, "progress"), t);
        glUniform1f(glGetUniformLocation(program, "opening"), 1.0f);
        glUniform1f(glGetUniformLocation(program, "uOpacity"), 1.0f);
        glUniform1f(glGetUniformLocation(program, "uSplit"), 0.0f);
        glUniform1f(glGetUniformLocation(program, "uAngle"), 0.0f);
        glUniform1f(glGetUniformLocation(program, "uOffsetX"), 0.0f);
        glUniform1f(glGetUniformLocation(program, "uOffsetY"), 0.0f);
        glUniform3f(glGetUniformLocation(program, "uInterior"), 0.102f, 0.0f, 0.173f);
        glUniform3f(glGetUniformLocation(program, "uVein"), 0.541f, 0.169f, 0.886f);
        glUniform3f(glGetUniformLocation(program, "uEdge"), 0.780f, 0.490f, 1.0f);

        glDrawArrays(GL_TRIANGLES, 0, 3);

        std::vector<unsigned char> pixels(
            static_cast<size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels.data());
        // RGBA -> RGB
        std::vector<unsigned char> rgb(pixels.size() / 4 * 3);
        for (size_t px = 0; px < rgb.size() / 3; ++px) {
            rgb[px * 3 + 0] = pixels[px * 4 + 0];
            rgb[px * 3 + 1] = pixels[px * 4 + 1];
            rgb[px * 3 + 2] = pixels[px * 4 + 2];
        }
        char path[512];
        std::snprintf(path, sizeof(path), "%s_%02zu_t%.2f.ppm",
                      prefix.c_str(), i, t);
        write_ppm(path, width, height, rgb);
        std::printf("wrote %s\n", path);
    }

    std::fflush(stdout);
    eglTerminate(display);
    return 0;
}
