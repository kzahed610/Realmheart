#pragma once

#include "ui/components/BaseWidget.hpp"
#include <gtk/gtk.h>
#include <string>
#include <vector>
#include <memory>

namespace realmheart::ui::components {

struct WorkspaceState {
    int id;
    std::string name;
    bool active;
    int windows;
};

class WorkspacePill : public ThemeableWidget {
public:
    WorkspacePill(const WorkspaceState& state);
    ~WorkspacePill() override = default;

    GtkWidget* get_widget() override;
    void update(const WorkspaceState& state);
    void apply_theme(const services::Palette& palette) override;

private:
    GtkWidget* label_ = nullptr;
};

} // namespace realmheart::ui::components
