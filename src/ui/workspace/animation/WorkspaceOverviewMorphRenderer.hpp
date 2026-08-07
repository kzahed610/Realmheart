#pragma once

#include "ui/workspace/animation/WorkspaceMorphShaderData.hpp"

#include <gtk/gtk.h>

#include <array>
#include <cstddef>
#include <string>

namespace realmheart::ui::workspace::animation {

struct WorkspaceMorphShaderPalette {
    std::array<float, 3> gold{{0.886F, 0.769F, 0.427F}};
    std::array<float, 3> starlight{{0.749F, 0.890F, 1.000F}};
    std::array<float, 3> astral{{0.353F, 0.290F, 0.612F}};
    std::array<float, 3> void_colour{{0.024F, 0.031F, 0.094F}};
};

// Transition-only GL overlay for the Elemental Workspace Overview. The live
// GSK canvas remains visible underneath at all times, so any capture, shader,
// context or draw failure automatically falls back to the geometry morph.
class WorkspaceOverviewMorphRenderer {
public:
    WorkspaceOverviewMorphRenderer();
    ~WorkspaceOverviewMorphRenderer();

    WorkspaceOverviewMorphRenderer(const WorkspaceOverviewMorphRenderer&) =
        delete;
    WorkspaceOverviewMorphRenderer& operator=(
        const WorkspaceOverviewMorphRenderer&
    ) = delete;

    [[nodiscard]] GtkWidget* widget() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool frame_ready() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::size_t captured_source_bytes() const noexcept;
    [[nodiscard]] std::size_t transient_source_bytes() const noexcept;

    [[nodiscard]] bool begin(
        GtkWidget* capture_parent,
        GtkWidget* source_child,
        bool opening,
        const WorkspaceMorphShaderGeometry& geometry,
        const WorkspaceMorphShaderPalette& palette = {},
        std::string* error = nullptr
    );

    void update(
        double progress,
        bool opening,
        const WorkspaceMorphShaderFrame& frame
    ) noexcept;
    void finish() noexcept;

private:
    struct State;
    State* state_ = nullptr;
};

} // namespace realmheart::ui::workspace::animation
