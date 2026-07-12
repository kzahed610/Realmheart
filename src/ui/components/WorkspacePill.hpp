#pragma once

#include "services/HyprlandWorkspaces.hpp"
#include "ui/components/BaseWidget.hpp"

namespace realmheart::ui::components {

using WorkspaceState = services::WorkspaceState;

class WorkspacePill : public BaseWidget {
public:
    explicit WorkspacePill(const WorkspaceState& state);
    ~WorkspacePill() override = default;

    GtkWidget* get_widget() override;
    void update(const WorkspaceState& state);
    [[nodiscard]] int workspace_id() const noexcept { return workspace_id_; }

private:
    GtkWidget* button_ = nullptr;
    int workspace_id_ = 0;
};

} // namespace realmheart::ui::components
