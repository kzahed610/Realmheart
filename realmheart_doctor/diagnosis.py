"""Read-only manual diagnosis and deterministic component aggregation.

Artifact evidence is not proof of runtime capability. Missing capability coverage
remains UNKNOWN rather than silently promoting an installation to healthy.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from realmheart_maintenance.manifest import ManifestRegistry
from realmheart_maintenance.version import RELEASE_VERSION

from .health import HealthCheckExecutor, HealthCheckResult, HealthStatus


class ComponentHealth(str, Enum):
    HEALTHY = "healthy"
    DEGRADED = "degraded"
    FAILED = "failed"
    UNKNOWN = "unknown"


EXIT_CODES = {ComponentHealth.HEALTHY: 0, ComponentHealth.DEGRADED: 1,
              ComponentHealth.FAILED: 2, ComponentHealth.UNKNOWN: 3}
CRITICAL_CATEGORIES = frozenset({"core", "fx", "essential"})


@dataclass(frozen=True)
class ComponentDiagnosis:
    id: str
    name: str
    category: str
    status: ComponentHealth
    checks: tuple[HealthCheckResult, ...]
    uncertainties: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, object]:
        return {"id": self.id, "name": self.name, "category": self.category,
                "status": self.status.value,
                "checks": [item.to_dict() for item in self.checks],
                "uncertainties": list(self.uncertainties)}


@dataclass(frozen=True)
class Diagnosis:
    release_version: str
    manifest_digest: str
    overall: ComponentHealth
    components: tuple[ComponentDiagnosis, ...]
    budget_exhausted: bool

    def to_dict(self) -> dict[str, object]:
        return {"format_version": 1, "doctor_version": RELEASE_VERSION, "release_version": self.release_version,
                "manifest_digest": self.manifest_digest,
                "overall": self.overall.value,
                "budget_exhausted": self.budget_exhausted,
                "components": [item.to_dict() for item in self.components]}


def diagnose(registry: ManifestRegistry, component: str | None = None, *,
             executor: HealthCheckExecutor | None = None) -> Diagnosis:
    if component is not None and component not in registry.components:
        raise ValueError("unknown component")
    selected = {component} if component is not None else set(registry.components)
    # Include required upstream components; preserve canonical topological order.
    pending = list(selected)
    while pending:
        for dependency in registry.components[pending.pop()].realmheart_dependencies:
            if dependency.required and dependency.id not in selected:
                selected.add(dependency.id)
                pending.append(dependency.id)
    ids = tuple(key for key, spec in registry.health_checks.items() if spec.component_id in selected)
    run = (executor or HealthCheckExecutor()).execute(
        registry, context="doctor_manual", max_cost="normal", check_ids=ids,
    )
    results: dict[str, ComponentDiagnosis] = {}
    for key in registry.component_order:
        if key not in selected:
            continue
        spec = registry.components[key]
        checks = tuple(item for item in run.results if item.component_id == key)
        required = []
        optional = []
        uncertainties = []
        for item in checks:
            definition = registry.health_checks[item.check_id]
            artifact = registry.artifacts.get(definition.artifact_id) if definition.artifact_id else None
            (optional if artifact is not None and not artifact.required else required).append(item.status)
        covered_artifacts = {registry.health_checks[item.check_id].artifact_id for item in checks}
        if any(artifact.required and artifact.component_id == key and artifact.id not in covered_artifacts
               for artifact in registry.artifacts.values()):
            uncertainties.append("required_artifact_coverage_missing")
        if not checks:
            uncertainties.append("no_declared_health_checks")
        capabilities = [item for item in registry.capabilities.values()
                        if item.component_id in (None, key) and "runtime" in item.lifecycle]
        if capabilities:
            uncertainties.append("runtime_capability_observation_pending")
        upstream = [results[item.id].status for item in spec.realmheart_dependencies if item.required]
        if HealthStatus.FAIL in required or ComponentHealth.FAILED in upstream:
            status = ComponentHealth.FAILED
        elif (uncertainties or any(item in {HealthStatus.UNKNOWN, HealthStatus.NOT_APPLICABLE}
                                   for item in required) or ComponentHealth.UNKNOWN in upstream):
            status = ComponentHealth.UNKNOWN
        elif any(item is not HealthStatus.PASS for item in optional) or ComponentHealth.DEGRADED in upstream:
            status = ComponentHealth.DEGRADED
        else:
            status = ComponentHealth.HEALTHY
        results[key] = ComponentDiagnosis(key, spec.name, spec.category, status, checks, tuple(uncertainties))
    values = tuple(results.values())
    if component is not None:
        overall = results[component].status
    elif any(item.status is ComponentHealth.FAILED and item.category in CRITICAL_CATEGORIES for item in values):
        overall = ComponentHealth.FAILED
    elif not values or any(item.status is ComponentHealth.UNKNOWN for item in values):
        overall = ComponentHealth.UNKNOWN
    elif any(item.status is not ComponentHealth.HEALTHY for item in values):
        overall = ComponentHealth.DEGRADED
    else:
        overall = ComponentHealth.HEALTHY
    return Diagnosis(registry.release_version, registry.digest, overall, values, run.budget_exhausted)


def render_diagnosis(result: Diagnosis, *, verbose: bool = False) -> str:
    lines = [f"Realmheart Doctor — {result.overall.value.upper()}"]
    for component in result.components:
        lines.append(f"  {component.status.value.upper():8} {component.id}")
        for item in component.checks:
            if verbose or item.status is not HealthStatus.PASS:
                lines.append(f"    {item.status.value}: {item.check_id} — {item.reason_code}")
        lines.extend(f"    unknown: {reason}" for reason in component.uncertainties)
    return "\n".join(lines)
