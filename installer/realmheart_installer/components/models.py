"""Phase-12 component-handler models.

The canonical component graph remains declarative in ``components/*.toml``.
These models describe installer-only execution behavior derived from the
approved :class:`InstallationPlan`; they never leak Python callables into the
shared Realmheart maintenance manifest.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import TYPE_CHECKING, Protocol

from ..models import ComponentResult, Reversibility

if TYPE_CHECKING:
    from ..planning.models import ArtifactAction, ConfigAction, PlannedComponent, PlannedHealthCheck, ServiceAction


class ComponentStepKind(str, Enum):
    """Meaningful mutation/verification classes used by a component handler."""

    ARTIFACT_COMMIT = "artifact_commit"
    CONFIGURATION = "configuration"
    SERVICES = "services"
    VERIFY = "verify"


@dataclass(frozen=True)
class RollbackRequirement:
    scope: str
    reversibility: Reversibility
    reason: str


@dataclass(frozen=True)
class ComponentFootprint:
    """All plan-owned state attributable to one meaningful component."""

    artifact_ids: tuple[str, ...]
    artifact_targets: tuple[str, ...]
    config_action_ids: tuple[str, ...]
    config_targets: tuple[str, ...]
    service_action_ids: tuple[str, ...]
    services: tuple[str, ...]
    build_unit_ids: tuple[str, ...]
    health_check_ids: tuple[str, ...]
    privileged_targets: tuple[str, ...]
    rollback_requirements: tuple[RollbackRequirement, ...]

    @property
    def mutates_live_state(self) -> bool:
        return bool(self.artifact_targets or self.config_targets or self.services)


@dataclass(frozen=True)
class ComponentHandlerSpec:
    """Resolved installer behavior for one canonical Realmheart component."""

    component: PlannedComponent
    footprint: ComponentFootprint
    artifact_actions: tuple[ArtifactAction, ...]
    config_actions: tuple[ConfigAction, ...]
    service_actions: tuple[ServiceAction, ...]
    health_checks: tuple[PlannedHealthCheck, ...]
    service_actions_owned_by_configuration: bool = False


@dataclass(frozen=True)
class HandlerStepResult:
    ok: bool
    reason: str
    operation_ids: tuple[str, ...] = ()
    warnings: tuple[str, ...] = ()
    error_code: str | None = None


@dataclass(frozen=True)
class ComponentProgressEvent:
    index: int
    total: int
    component_id: str
    display_name: str
    category: str
    stage: str
    state: str
    detail: str | None = None


@dataclass(frozen=True)
class ComponentExecutionReport:
    results: tuple[ComponentResult, ...]
    progress: tuple[ComponentProgressEvent, ...]
    halted: bool = False
    halt_reason: str | None = None

    @property
    def failed(self) -> tuple[ComponentResult, ...]:
        from ..models import ComponentState
        return tuple(item for item in self.results if item.state is ComponentState.FAILED)

    @property
    def blocked(self) -> tuple[ComponentResult, ...]:
        from ..models import ComponentState
        return tuple(item for item in self.results if item.state is ComponentState.BLOCKED)


class ComponentMutationBackend(Protocol):
    """Mutation boundary consumed by Phase-12 component handlers.

    The handler decides *which* Realmheart-owned operations belong to a
    component and in what order.  The backend performs those operations through
    the transaction-aware primitives supplied by later live-execution phases.
    Tests use a recording backend, which lets Phase 12 prove orchestration
    without touching the user's machine.
    """

    def commit_artifacts(
        self,
        component: PlannedComponent,
        actions: tuple[ArtifactAction, ...],
    ) -> HandlerStepResult: ...

    def apply_configuration(
        self,
        component: PlannedComponent,
        actions: tuple[ConfigAction, ...],
    ) -> HandlerStepResult: ...

    def apply_services(
        self,
        component: PlannedComponent,
        actions: tuple[ServiceAction, ...],
    ) -> HandlerStepResult: ...

    def verify_component(
        self,
        component: PlannedComponent,
        checks: tuple[PlannedHealthCheck, ...],
    ) -> HandlerStepResult: ...

    def rollback_component(
        self,
        component: PlannedComponent,
        operation_ids: tuple[str, ...],
        requirements: tuple[RollbackRequirement, ...],
    ) -> HandlerStepResult: ...
