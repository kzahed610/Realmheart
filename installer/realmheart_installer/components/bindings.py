"""Installer-only behavior registry keyed by shared stable component IDs."""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable

from realmheart_maintenance.manifest import ManifestError, ManifestRegistry
from ..models import ComponentResult
from .models import HandlerStepResult

InstallCallable = Callable[[Any], ComponentResult]
VerifyCallable = Callable[[Any, ComponentResult], ComponentResult]
RollbackCallable = Callable[[Any, ComponentResult], HandlerStepResult]


@dataclass(frozen=True)
class InstallerComponentBinding:
    component_id: str
    install: InstallCallable
    verify_extra: VerifyCallable | None = None
    rollback: RollbackCallable | None = None


class InstallerBindingRegistry:
    def __init__(self, bindings: tuple[InstallerComponentBinding, ...] = ()) -> None:
        self._bindings: dict[str, InstallerComponentBinding] = {}
        for binding in bindings:
            if binding.component_id in self._bindings:
                raise ManifestError(f"duplicate installer binding: {binding.component_id}")
            self._bindings[binding.component_id] = binding

    def get(self, component_id: str) -> InstallerComponentBinding | None:
        return self._bindings.get(component_id)

    def validate(self, manifest: ManifestRegistry) -> None:
        missing = sorted(
            component.id for component in manifest.components.values()
            if component.requires_installer_binding and component.id not in self._bindings
        )
        if missing:
            raise ManifestError("required installer binding(s) missing: " + ", ".join(missing))
        unknown = sorted(set(self._bindings) - set(manifest.components))
        if unknown:
            raise ManifestError("installer binding(s) reference unknown component: " + ", ".join(unknown))


def _phase12_binding(component_id: str) -> InstallerComponentBinding:
    """Create the special-behavior binding for one canonical component.

    The manifest marks these components because generic artifact/service commit
    alone is insufficient. Their actual implementation now lives in the Phase-12
    handler layer rather than the old fail-closed reservation stub.
    """

    def install(context: Any) -> ComponentResult:
        from .handlers import ComponentHandlerContext, install_component
        if not isinstance(context, ComponentHandlerContext):
            raise TypeError(f"{component_id} requires a Phase-12 ComponentHandlerContext")
        if context.spec.component.id != component_id:
            raise ValueError(
                f"installer binding for {component_id} received context for {context.spec.component.id}"
            )
        return install_component(context)

    def rollback(context: Any, result: ComponentResult) -> HandlerStepResult:
        from .handlers import ComponentHandlerContext, rollback_component
        if not isinstance(context, ComponentHandlerContext):
            raise TypeError(f"{component_id} requires a Phase-12 ComponentHandlerContext")
        return rollback_component(context, result)

    return InstallerComponentBinding(component_id, install, rollback=rollback)


def default_installer_bindings() -> InstallerBindingRegistry:
    """Return custom behavior bindings required by the current manifest.

    Generic components are resolved by the Phase-12 handler engine directly.
    Only components whose canonical manifest declares
    ``requires_installer_binding`` are named here; this is the intentional
    installer-specific behavior registry described by the architecture.
    """

    return InstallerBindingRegistry((
        _phase12_binding("hypr-integration"),
        _phase12_binding("lockscreen-auth"),
        _phase12_binding("terminal"),
    ))
