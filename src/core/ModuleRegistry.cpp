#include "core/ModuleRegistry.hpp"

#include <sstream>
#include <utility>

namespace realmheart::core {

void ModuleRegistry::add(ModuleDescriptor descriptor) {
    modules_.push_back(std::move(descriptor));
}

const std::vector<ModuleDescriptor>& ModuleRegistry::modules() const {
    return modules_;
}

std::string phase_name(ModulePhase phase) {
    switch (phase) {
        case ModulePhase::CoreService: return "core-service";
        case ModulePhase::Surface: return "surface";
        case ModulePhase::Widget: return "widget";
        case ModulePhase::Overlay: return "overlay";
        case ModulePhase::Lock: return "lock";
    }
    return "unknown";
}

std::string ModuleRegistry::describe() const {
    std::ostringstream out;
    out << "Realmheart module registry\n";
    out << "==========================\n";
    for (const auto& module : modules_) {
        out << "- " << module.id << " [" << phase_name(module.phase) << "] " << module.label;
        if (!module.confirmed_scope) out << " (scope-unconfirmed)";
        out << '\n';
        if (!module.service_dependencies.empty()) {
            out << "  services:";
            for (const auto& dependency : module.service_dependencies) out << ' ' << dependency;
            out << '\n';
        }
    }
    return out.str();
}

} // namespace realmheart::core
