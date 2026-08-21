#include "ui/lockscreen/CrystalShaderRenderer.hpp"

#include "ui/lockscreen/ShaderManager.hpp"

#include <epoxy/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace realmheart::ui::lockscreen {
namespace {

constexpr float kBaseOpacity = 1.0F;

} // namespace

struct CrystalShaderRenderer::State {
    GtkWidget* gl_area = nullptr;
    std::shared_ptr<ShaderManager> shaders;

    bool active = false;
    bool frame_rendered_ = false;
    bool opening = true;
    float progress = 0.0F;
    float split = 0.0F;
    float angle = 0.0F;
    float offset_x = 0.0F;
    float offset_y = 0.0F;
    float opacity = 1.0F;

    GLuint vertex_array = 0;

    void release_gl_resources() noexcept {
        if (gl_area == nullptr || !gtk_widget_get_realized(gl_area)) {
            vertex_array = 0;
            return;
        }

        gtk_gl_area_make_current(GTK_GL_AREA(gl_area));
        if (gtk_gl_area_get_error(GTK_GL_AREA(gl_area)) != nullptr) return;

        if (vertex_array != 0) glDeleteVertexArrays(1, &vertex_array);
        vertex_array = 0;
    }

    void fail(std::string message) noexcept {
        std::cerr << "[BrokenSeal] crystal renderer GL fallback: "
                  << message << '\n';
        active = false;
        if (gl_area != nullptr) {
            gtk_widget_set_opacity(gl_area, 0.0);
            gtk_widget_set_visible(gl_area, FALSE);
        }
    }

    gboolean render(GtkGLArea* area) noexcept {
        if (!active) return TRUE;

        if (const GError* gl_error = gtk_gl_area_get_error(area);
            gl_error != nullptr) {
            fail(gl_error->message != nullptr
                ? gl_error->message
                : "OpenGL context error");
            return TRUE;
        }

        if (shaders == nullptr || !shaders->ensure_loaded()) {
            fail("crystal shader unavailable");
            return TRUE;
        }
        const GLuint program = shaders->crystal_program();
        if (program == 0) return TRUE;

        if (vertex_array == 0) {
            glGenVertexArrays(1, &vertex_array);
        }

        const int scale = std::max(
            gtk_widget_get_scale_factor(GTK_WIDGET(area)),
            1
        );
        const int width = std::max(
            gtk_widget_get_width(GTK_WIDGET(area)) * scale,
            1
        );
        const int height = std::max(
            gtk_widget_get_height(GTK_WIDGET(area)) * scale,
            1
        );

        glViewport(0, 0, width, height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(program);
        glBindVertexArray(vertex_array);

        glUniform1f(glGetUniformLocation(program, "progress"), progress);
        glUniform2f(
            glGetUniformLocation(program, "resolution"),
            static_cast<float>(width),
            static_cast<float>(height)
        );
        glUniform1f(
            glGetUniformLocation(program, "opening"),
            opening ? 1.0F : 0.0F
        );
        glUniform1f(glGetUniformLocation(program, "uOpacity"), opacity);
        glUniform1f(glGetUniformLocation(program, "uSplit"), split);
        glUniform1f(glGetUniformLocation(program, "uAngle"), angle);
        glUniform1f(glGetUniformLocation(program, "uOffsetX"), offset_x);
        glUniform1f(glGetUniformLocation(program, "uOffsetY"), offset_y);

        constexpr std::array<float, 3> kInterior{0.102F, 0.0F, 0.173F};
        constexpr std::array<float, 3> kVein{0.541F, 0.169F, 0.886F};
        constexpr std::array<float, 3> kEdge{0.780F, 0.490F, 1.0F};
        glUniform3fv(glGetUniformLocation(program, "uInterior"), 1, kInterior.data());
        glUniform3fv(glGetUniformLocation(program, "uVein"), 1, kVein.data());
        glUniform3fv(glGetUniformLocation(program, "uEdge"), 1, kEdge.data());

        glDrawArrays(GL_TRIANGLES, 0, 3);

        glBindVertexArray(0);
        glUseProgram(0);

        const GLenum draw_error = glGetError();
        if (draw_error != GL_NO_ERROR) {
            fail("OpenGL draw failed with error " +
                 std::to_string(static_cast<unsigned int>(draw_error)));
            return TRUE;
        }

        frame_rendered_ = true;
        return TRUE;
    }
};

CrystalShaderRenderer::CrystalShaderRenderer(std::shared_ptr<ShaderManager> shaders)
    : state_(new State) {
    state_->shaders = std::move(shaders);
    state_->gl_area = gtk_gl_area_new();
    g_object_ref_sink(state_->gl_area);
    gtk_widget_set_hexpand(state_->gl_area, TRUE);
    gtk_widget_set_vexpand(state_->gl_area, TRUE);
    gtk_widget_set_halign(state_->gl_area, GTK_ALIGN_FILL);
    gtk_widget_set_valign(state_->gl_area, GTK_ALIGN_FILL);
    gtk_widget_set_can_target(state_->gl_area, FALSE);
    gtk_widget_set_focusable(state_->gl_area, FALSE);
    gtk_widget_add_css_class(state_->gl_area, "realmheart-broken-seal-crystal");
    gtk_widget_remove_css_class(state_->gl_area, "background");
    gtk_widget_set_visible(state_->gl_area, FALSE);
    gtk_widget_set_opacity(state_->gl_area, 0.0);

    gtk_gl_area_set_allowed_apis(
        GTK_GL_AREA(state_->gl_area),
        GDK_GL_API_GLES
    );
    gtk_gl_area_set_required_version(GTK_GL_AREA(state_->gl_area), 3, 0);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(state_->gl_area), FALSE);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(state_->gl_area), FALSE);
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(state_->gl_area), FALSE);

    g_signal_connect(
        state_->gl_area,
        "render",
        G_CALLBACK(+[](GtkGLArea* area, GdkGLContext*, gpointer data) -> gboolean {
            return static_cast<State*>(data)->render(area);
        }),
        state_
    );
    g_signal_connect(
        state_->gl_area,
        "unrealize",
        G_CALLBACK(+[](GtkWidget*, gpointer data) {
            static_cast<State*>(data)->release_gl_resources();
        }),
        state_
    );
}

CrystalShaderRenderer::~CrystalShaderRenderer() {
    if (state_ == nullptr) return;
    finish();
    state_->release_gl_resources();
    if (state_->gl_area != nullptr) {
        g_signal_handlers_disconnect_by_data(state_->gl_area, state_);
        g_object_unref(state_->gl_area);
        state_->gl_area = nullptr;
    }
    delete state_;
    state_ = nullptr;
}

GtkWidget* CrystalShaderRenderer::widget() const noexcept {
    return state_ != nullptr ? state_->gl_area : nullptr;
}

void CrystalShaderRenderer::update(const CrystalSceneFrame& frame) noexcept {
    if (state_ == nullptr) return;
    state_->progress = static_cast<float>(std::clamp(
        std::isfinite(frame.progress) ? frame.progress : 0.0,
        0.0,
        1.0
    ));
    state_->opening = frame.opening;
    state_->split = static_cast<float>(std::clamp(
        std::isfinite(frame.split) ? frame.split : 0.0,
        0.0,
        1.0
    ));
    state_->angle = static_cast<float>(std::clamp(
        std::isfinite(frame.angle) ? frame.angle : 0.0,
        0.0,
        3.14159265358979323846
    ));
    state_->offset_x = static_cast<float>(std::clamp(
        std::isfinite(frame.offset_x) ? frame.offset_x : 0.0,
        0.0,
        0.5
    ));
    state_->offset_y = static_cast<float>(std::clamp(
        std::isfinite(frame.offset_y) ? frame.offset_y : 0.0,
        -0.5,
        0.5
    ));
    if (!state_->active) {
        state_->active = true;
        state_->frame_rendered_ = false;
        if (state_->gl_area != nullptr) {
            gtk_widget_set_visible(state_->gl_area, TRUE);
            gtk_widget_set_opacity(state_->gl_area, kBaseOpacity);
        }
    }
    if (state_->gl_area != nullptr) {
        gtk_gl_area_queue_render(GTK_GL_AREA(state_->gl_area));
    }
}

bool CrystalShaderRenderer::frame_rendered() const noexcept {
    return state_ != nullptr && state_->frame_rendered_;
}

void CrystalShaderRenderer::set_opacity(double opacity) noexcept {
    if (state_ == nullptr || state_->gl_area == nullptr) return;
    const double safe_opacity = std::isfinite(opacity)
        ? std::clamp(opacity, 0.0, 1.0)
        : 1.0;
    state_->opacity = static_cast<float>(safe_opacity);
    gtk_widget_set_opacity(state_->gl_area, safe_opacity);
}

void CrystalShaderRenderer::finish() noexcept {
    if (state_ == nullptr) return;
    state_->active = false;
    state_->frame_rendered_ = false;
    if (state_->gl_area != nullptr) {
        gtk_widget_set_opacity(state_->gl_area, 0.0);
        gtk_widget_set_visible(state_->gl_area, FALSE);
    }
}

} // namespace realmheart::ui::lockscreen
