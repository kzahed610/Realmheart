"""Meaningful manifest-driven Realmheart component handlers.

Phase 12 turns the declarative component graph into installer execution units.
The handler layer owns orchestration and failure attribution; the mutation
backend owns the actual transaction-aware filesystem/service implementation.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from realmheart_maintenance.manifest import ManifestRegistry

from ..models import ComponentResult, ComponentState, Reversibility
from ..planning.models import (
    ArtifactAction,
    ArtifactCommitClass,
    ConfigAction,
    ConfigActionKind,
    InstallationPlan,
    PlannedComponent,
    PlannedHealthCheck,
    ServiceAction,
)
from .models import (
    ComponentFootprint,
    ComponentHandlerSpec,
    ComponentMutationBackend,
    HandlerStepResult,
    RollbackRequirement,
)


@dataclass(frozen=True)
class ComponentHandlerContext:
    plan: InstallationPlan
    manifest: ManifestRegistry
    spec: ComponentHandlerSpec
    backend: ComponentMutationBackend


def resolve_component_handler_specs(
    plan: InstallationPlan,
    manifest: ManifestRegistry,
) -> tuple[ComponentHandlerSpec, ...]:
    """Resolve all handler footprints directly from the approved plan.

    No second component table exists here.  Stable IDs/names/dependencies come
    from the shared manifest and every mutation target comes from the exact
    serialized InstallationPlan the user inspected.
    """

    plan_by_id = {item.id: item for item in plan.components}
    if tuple(plan_by_id) != tuple(item.id for item in plan.components):
        raise ValueError("duplicate planned component IDs")

    manifest_ids = tuple(manifest.component_order)
    plan_ids = tuple(item.id for item in plan.components)
    if plan_ids != manifest_ids:
        raise ValueError("InstallationPlan component order does not match canonical manifest")

    specs: list[ComponentHandlerSpec] = []
    for component_id in manifest_ids:
        component = plan_by_id[component_id]
        artifact_actions = tuple(item for item in plan.artifact_actions if item.component_id == component_id)
        config_actions = tuple(item for item in plan.config_actions if item.component_id == component_id)
        service_actions = tuple(item for item in plan.service_actions if item.component_id == component_id)
        health_checks = tuple(item for item in plan.health_checks if item.component_id == component_id)

        # User/shared/generated artifacts are committed through their explicit
        # configuration contracts rather than copied a second time as generic
        # artifacts. Native/system payload remains an artifact commit.
        generic_artifact_actions = tuple(
            item for item in artifact_actions
            if item.commit_class in {ArtifactCommitClass.PRIVILEGED_COMMIT, ArtifactCommitClass.STAGED_PAYLOAD}
        )

        rollback = _rollback_requirements(generic_artifact_actions, config_actions, service_actions)
        privileged_targets = tuple(item.target for item in artifact_actions if item.privileged)
        footprint = ComponentFootprint(
            artifact_ids=tuple(item.artifact_id for item in artifact_actions),
            artifact_targets=tuple(item.target for item in artifact_actions),
            config_action_ids=tuple(item.id for item in config_actions),
            config_targets=tuple(item.target for item in config_actions),
            service_action_ids=tuple(item.id for item in service_actions),
            services=tuple(item.service for item in service_actions),
            build_unit_ids=component.build_units,
            # The installation plan carries only checks selected for the
            # install_verify context.  Keep the complete manifest footprint
            # visible here as well so Doctor-only checks remain part of the
            # component's declared ownership without being executed by the
            # Installer verifier.
            health_check_ids=tuple(
                item.id
                for item in manifest.health_checks.values()
                if item.component_id == component_id
            ),
            privileged_targets=privileged_targets,
            rollback_requirements=rollback,
        )
        specs.append(ComponentHandlerSpec(
            component=component,
            footprint=footprint,
            artifact_actions=generic_artifact_actions,
            config_actions=config_actions,
            service_actions=service_actions,
            health_checks=health_checks,
            # Phase-11 terminal integration owns its path/service activation as
            # one rollback domain. Running the service plan again would double
            # activate the watcher.
            service_actions_owned_by_configuration=(component_id == "terminal"),
        ))
    return tuple(specs)


def install_component(context: ComponentHandlerContext) -> ComponentResult:
    """Execute one meaningful Realmheart component through the backend."""

    spec = context.spec
    component = spec.component
    started = datetime.now(timezone.utc)
    operation_ids: list[str] = []
    warnings: list[str] = []

    def consume(result: HandlerStepResult) -> ComponentResult | None:
        operation_ids.extend(result.operation_ids)
        warnings.extend(result.warnings)
        if result.ok:
            return None
        return ComponentResult(
            component_id=component.id,
            state=ComponentState.FAILED,
            stage=component.stage,
            started_at=started,
            finished_at=datetime.now(timezone.utc),
            reason=result.reason,
            error_code=result.error_code or "component_step_failed",
            warnings=tuple(warnings),
            operation_ids=tuple(operation_ids),
        )

    if spec.artifact_actions:
        failed = consume(context.backend.commit_artifacts(component, spec.artifact_actions))
        if failed:
            return failed

    mutating_config = tuple(item for item in spec.config_actions if item.will_mutate)
    if mutating_config:
        failed = consume(context.backend.apply_configuration(component, spec.config_actions))
        if failed:
            return failed

    if spec.service_actions and not spec.service_actions_owned_by_configuration:
        failed = consume(context.backend.apply_services(component, spec.service_actions))
        if failed:
            return failed

    # Phase 12 provides handler-local verification hooks. Phase 13 expands this
    # into the full installation verification/health engine. Verification is
    # observational, so any backend operation IDs are deliberately not adopted
    # into the component mutation footprint.
    verification = context.backend.verify_component(component, spec.health_checks)
    warnings.extend(verification.warnings)
    if not verification.ok:
        return ComponentResult(
            component_id=component.id,
            state=ComponentState.FAILED,
            stage=component.stage,
            started_at=started,
            finished_at=datetime.now(timezone.utc),
            reason=verification.reason,
            error_code=verification.error_code or "component_verify_failed",
            warnings=tuple(warnings),
            operation_ids=tuple(operation_ids),
        )

    if not spec.artifact_actions and not mutating_config and not spec.service_actions:
        reason = "capability/integration component satisfied; no independent live artifact commit"
    else:
        reason = "component install handler completed"
    return ComponentResult(
        component_id=component.id,
        state=ComponentState.PASS,
        stage=component.stage,
        started_at=started,
        finished_at=datetime.now(timezone.utc),
        reason=reason,
        warnings=tuple(warnings),
        operation_ids=tuple(operation_ids),
    )


def rollback_component(context: ComponentHandlerContext, result: ComponentResult) -> HandlerStepResult:
    """Delegate rollback using the exact footprint attached to this handler."""

    return context.backend.rollback_component(
        context.spec.component,
        result.operation_ids,
        context.spec.footprint.rollback_requirements,
    )


def _rollback_requirements(
    artifact_actions: tuple[ArtifactAction, ...],
    config_actions: tuple[ConfigAction, ...],
    service_actions: tuple[ServiceAction, ...],
) -> tuple[RollbackRequirement, ...]:
    requirements: list[RollbackRequirement] = []
    for action in artifact_actions:
        if action.privileged:
            reversibility = Reversibility.GUARDED
            reason = "privileged artifact commit requires preimage + compare-before-restore"
        else:
            reversibility = Reversibility.EXACT
            reason = "Realmheart-owned artifact commit restores its transaction preimage exactly"
        requirements.append(RollbackRequirement(action.target, reversibility, reason))

    for action in config_actions:
        if not action.will_mutate:
            continue
        requirements.append(RollbackRequirement(
            action.target,
            action.reversibility,
            f"configuration action {action.id} declares {action.reversibility.value} rollback",
        ))

    for action in service_actions:
        requirements.append(RollbackRequirement(
            action.service,
            Reversibility.BEST_EFFORT,
            "restore captured user-systemd enabled/active state after file rollback",
        ))

    # Stable ordering and deduplication keep support output deterministic.
    seen: set[tuple[str, Reversibility]] = set()
    deduped: list[RollbackRequirement] = []
    for item in requirements:
        key = (item.scope, item.reversibility)
        if key in seen:
            continue
        seen.add(key)
        deduped.append(item)
    return tuple(deduped)
