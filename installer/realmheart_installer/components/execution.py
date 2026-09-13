"""Dependency-aware component execution for manifest-driven Realmheart installs."""
from __future__ import annotations

from datetime import datetime, timezone
from typing import Any, Callable

from realmheart_maintenance.manifest import ManifestRegistry
from ..models import ComponentResult, ComponentState
from ..planning.models import InstallationPlan
from .bindings import InstallerBindingRegistry, default_installer_bindings
from .handlers import (
    ComponentHandlerContext,
    install_component,
    resolve_component_handler_specs,
    rollback_component,
)
from .models import ComponentExecutionReport, ComponentMutationBackend, ComponentProgressEvent

ProgressCallback = Callable[[ComponentProgressEvent], None]


def execute_component_graph(
    manifest: ManifestRegistry,
    bindings: InstallerBindingRegistry,
    context: Any,
) -> tuple[ComponentResult, ...]:
    """Legacy/minimal graph executor retained for focused graph tests.

    Phase 12's real installer path is :func:`execute_installation_components`,
    which resolves exact plan footprints and handler behavior.  This small
    executor remains useful for graph semantics and synthetic consumers.
    """

    bindings.validate(manifest)
    results: dict[str, ComponentResult] = {}
    ordered: list[ComponentResult] = []
    for component_id in manifest.component_order:
        component = manifest.components[component_id]
        blocked_by = tuple(
            dep.id for dep in component.realmheart_dependencies
            if dep.required and dep.id in results and results[dep.id].state in {ComponentState.FAILED, ComponentState.BLOCKED}
        )
        if blocked_by:
            result = ComponentResult(component_id, ComponentState.BLOCKED, component.stage, reason="required Realmheart dependency failed", blocked_by=blocked_by)
        else:
            binding = bindings.get(component_id)
            if binding is None:
                now = datetime.now(timezone.utc)
                result = ComponentResult(component_id, ComponentState.PASS, component.stage, started_at=now, finished_at=now, reason="generic manifest-driven component")
            else:
                try:
                    result = binding.install(context)
                except Exception as exc:  # graph test path contains ordinary component failures.
                    now = datetime.now(timezone.utc)
                    result = ComponentResult(component_id, ComponentState.FAILED, component.stage, started_at=now, finished_at=now, reason=str(exc), error_code="component_exception")
                if result.component_id != component_id:
                    raise ValueError(f"installer binding for {component_id} returned result for {result.component_id}")
        results[component_id] = result
        ordered.append(result)
    return tuple(ordered)


def execute_installation_components(
    plan: InstallationPlan,
    manifest: ManifestRegistry,
    backend: ComponentMutationBackend,
    *,
    bindings: InstallerBindingRegistry | None = None,
    package_actions_applied: bool = False,
    progress: ProgressCallback | None = None,
) -> ComponentExecutionReport:
    """Execute all meaningful component handlers in canonical topological order.

    A normal component failure is contained: dependent branches become
    ``BLOCKED`` while unrelated components continue.  Global transaction safety
    failures should be raised by the backend and are intentionally *not* hidden
    here; the outer live transaction executor owns that policy.
    """

    if not plan.ready:
        raise ValueError("refusing component execution for a blocked InstallationPlan")
    if plan.manifest_digest != manifest.digest:
        raise ValueError("InstallationPlan manifest digest no longer matches canonical manifest")

    binding_registry = bindings or default_installer_bindings()
    binding_registry.validate(manifest)
    specs = resolve_component_handler_specs(plan, manifest)
    spec_by_id = {item.component.id: item for item in specs}
    result_by_id: dict[str, ComponentResult] = {}
    ordered: list[ComponentResult] = []
    events: list[ComponentProgressEvent] = []
    total = len(specs)

    def emit(event: ComponentProgressEvent) -> None:
        events.append(event)
        if progress is not None:
            progress(event)

    for index, planned in enumerate(plan.components, start=1):
        spec = spec_by_id[planned.id]
        required_deps = tuple(manifest.components[planned.id].realmheart_dependencies)
        blocked_by = tuple(
            dep.id for dep in required_deps
            if dep.required and dep.id in result_by_id
            and result_by_id[dep.id].state in {ComponentState.FAILED, ComponentState.BLOCKED}
        )

        if blocked_by:
            result = ComponentResult(
                planned.id,
                ComponentState.BLOCKED,
                planned.stage,
                reason="required Realmheart dependency failed or was blocked",
                blocked_by=blocked_by,
            )
            emit(_event(index, total, planned, result.state, result.reason))
        elif planned.dependency_state == "blocked":
            unresolved = {item.capability_id for item in plan.package_plan.unresolved}
            blocked_external = tuple(item for item in planned.capability_ids if item in unresolved)
            result = ComponentResult(
                planned.id,
                ComponentState.BLOCKED,
                planned.stage,
                reason="required external dependency is unresolved",
                blocked_by=blocked_external,
            )
            emit(_event(index, total, planned, result.state, result.reason))
        elif planned.dependency_state == "pending_package" and not package_actions_applied:
            pending = {
                provider.capability_id for provider in plan.package_plan.providers
                if provider.resolution.value in {"install", "upgrade"}
            }
            blocked_external = tuple(item for item in planned.capability_ids if item in pending)
            result = ComponentResult(
                planned.id,
                ComponentState.BLOCKED,
                planned.stage,
                reason="required package actions have not been applied yet",
                blocked_by=blocked_external,
            )
            emit(_event(index, total, planned, result.state, result.reason))
        else:
            emit(_event(index, total, planned, ComponentState.RUNNING, "install handler running"))
            context = ComponentHandlerContext(plan, manifest, spec, backend)
            binding = binding_registry.get(planned.id)
            try:
                result = binding.install(context) if binding is not None else install_component(context)
                if binding is not None and binding.verify_extra is not None and result.state is ComponentState.PASS:
                    result = binding.verify_extra(context, result)
            except Exception:
                # Backends may raise explicit global-safety exceptions.  Do not
                # collapse them into a local component failure here.
                raise
            if result.component_id != planned.id:
                raise ValueError(f"handler for {planned.id} returned result for {result.component_id}")
            emit(_event(index, total, planned, result.state, result.reason))

        result_by_id[planned.id] = result
        ordered.append(result)

    return ComponentExecutionReport(tuple(ordered), tuple(events))


def rollback_installation_components(
    report: ComponentExecutionReport,
    plan: InstallationPlan,
    manifest: ManifestRegistry,
    backend: ComponentMutationBackend,
    *,
    bindings: InstallerBindingRegistry | None = None,
) -> tuple[str, ...]:
    """Rollback successfully-mutated component handlers in reverse graph order.

    Returns human-readable rollback errors rather than hiding them.  The outer
    transaction layer can promote any returned error to its rollback-failed
    health state.
    """

    binding_registry = bindings or default_installer_bindings()
    binding_registry.validate(manifest)
    specs = {item.component.id: item for item in resolve_component_handler_specs(plan, manifest)}
    results = {item.component_id: item for item in report.results}
    errors: list[str] = []

    for planned in reversed(plan.components):
        result = results.get(planned.id)
        if result is None or not result.operation_ids:
            continue
        context = ComponentHandlerContext(plan, manifest, specs[planned.id], backend)
        binding = binding_registry.get(planned.id)
        try:
            rollback_result = (
                binding.rollback(context, result)
                if binding is not None and binding.rollback is not None
                else rollback_component(context, result)
            )
        except Exception as exc:
            errors.append(f"{planned.id}: {type(exc).__name__}: {exc}")
            continue
        if not rollback_result.ok:
            errors.append(f"{planned.id}: {rollback_result.reason}")
    return tuple(errors)


def _event(
    index: int,
    total: int,
    planned: Any,
    state: ComponentState,
    detail: str | None,
) -> ComponentProgressEvent:
    return ComponentProgressEvent(
        index=index,
        total=total,
        component_id=planned.id,
        display_name=planned.name,
        category=planned.category,
        stage=planned.stage,
        state=state.value,
        detail=detail,
    )
