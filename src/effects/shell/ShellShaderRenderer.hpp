#pragma once

#include "effects/core/EffectFrame.hpp"

#include <gtk/gtk.h>

#include <array>
#include <string>

namespace realmheart::effects::shell {

struct ShaderPalette {
    std::array<float, 3> gold{0.886F, 0.769F, 0.427F};
    std::array<float, 3> starlight{0.749F, 0.890F, 1.000F};
    std::array<float, 3> astral{0.353F, 0.290F, 0.612F};
    std::array<float, 3> void_colour{0.024F, 0.031F, 0.094F};
};

// A pre-created GtkGLArea renderer for Realmheart-owned GTK surfaces.
// The widget must be inserted into a normal GTK container before that surface
// is mapped. begin() captures the live child, then swaps to GL only after the
// first completed frame. finish() releases transition-only texture storage.
// `corner_radius` is an optional compositor/backend fallback; GTK captures can
// pass zero and let the source texture's alpha define the exact target shape.
class ShellShaderRenderer {
public:
    ShellShaderRenderer();
    ~ShellShaderRenderer();

    ShellShaderRenderer(const ShellShaderRenderer&) = delete;
    ShellShaderRenderer& operator=(const ShellShaderRenderer&) = delete;

    [[nodiscard]] GtkWidget* widget() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool frame_ready() const noexcept;

    [[nodiscard]] bool begin(
        GtkWidget* capture_parent,
        GtkWidget* source_child,
        EffectId effect,
        bool opening,
        double corner_radius,
        const ShaderPalette& palette,
        std::string* error = nullptr
    );

    void update(double timeline_progress, bool opening) noexcept;
    void finish() noexcept;

private:
    struct State;
    State* state_ = nullptr;
};

} // namespace realmheart::effects::shell
