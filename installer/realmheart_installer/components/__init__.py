"""Installer behavior bindings layered on the shared manifest."""
from .bindings import InstallerComponentBinding, InstallerBindingRegistry, default_installer_bindings
from .execution import execute_component_graph, execute_installation_components, rollback_installation_components
from .handlers import ComponentHandlerContext, install_component, resolve_component_handler_specs, rollback_component
from .models import (
    ComponentExecutionReport,
    ComponentFootprint,
    ComponentHandlerSpec,
    ComponentMutationBackend,
    ComponentProgressEvent,
    HandlerStepResult,
    RollbackRequirement,
)

__all__ = [
    "InstallerComponentBinding",
    "InstallerBindingRegistry",
    "default_installer_bindings",
    "execute_component_graph",
    "execute_installation_components",
    "rollback_installation_components",
    "ComponentHandlerContext",
    "install_component",
    "rollback_component",
    "resolve_component_handler_specs",
    "ComponentExecutionReport",
    "ComponentFootprint",
    "ComponentHandlerSpec",
    "ComponentMutationBackend",
    "ComponentProgressEvent",
    "HandlerStepResult",
    "RollbackRequirement",
]
