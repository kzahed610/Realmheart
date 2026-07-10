#pragma once

#include <string>
#include <vector>

namespace realmheart::core {

enum class ModulePhase {
    CoreService,
    Surface,
    Widget,
    Overlay,
    Lock
};

struct ModuleDescriptor {
    std::string id;
    std::string label;
    ModulePhase phase;
    std::vector<std::string> service_dependencies;
    bool confirmed_scope = true;
};

class ModuleRegistry {
public:
    void add(ModuleDescriptor descriptor);
    [[nodiscard]] const std::vector<ModuleDescriptor>& modules() const;
    [[nodiscard]] std::string describe() const;

private:
    std::vector<ModuleDescriptor> modules_;
};

std::string phase_name(ModulePhase phase);

} // namespace realmheart::core
