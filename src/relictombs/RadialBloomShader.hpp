#pragma once

#include <gtk/gtk.h>
#include <epoxy/gl.h>
#include <string>
#include <optional>

namespace realmheart::relictombs {

class RadialBloomShader {
public:
    RadialBloomShader();
    ~RadialBloomShader();

    [[nodiscard]] bool compile();
    void set_origin(float x, float y);
    void set_radius(float radius);
    void set_alpha(float alpha);
    void draw(GLuint wallpaper_texture, int width, int height);
    void use() const;
    void unuse() const;

    [[nodiscard]] bool is_valid() const { return program_ != 0; }

private:
    GLuint program_ = 0;
    GLint u_origin_ = -1;
    GLint u_radius_ = -1;
    GLint u_wallpaper_ = -1;
    GLint u_alpha_ = -1;
    GLint a_position_ = -1;
    GLint a_texcoord_ = -1;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;

    [[nodiscard]] bool load_shader_source(const std::string& path, std::string& out_source);
    [[nodiscard]] GLuint compile_shader(GLenum type, std::string source);
    [[nodiscard]] std::string read_shader_file();
};

} // namespace realmheart::relictombs