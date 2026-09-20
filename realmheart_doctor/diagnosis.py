"""Read-only manual diagnosis and deterministic component aggregation.

Artifact evidence is not proof of runtime capability. Missing capability coverage
remains UNKNOWN rather than silently promoting an installation to healthy.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

from realmheart_maintenance.fingerprint import (
    FingerprintLimitExceeded,
    FingerprintObservationError,
    MAX_FINGERPRINT_BYTES,
    read_regular_file,
)
from realmheart_maintenance.manifest import (
    ManifestRegistry,
    ParsedVersion,
    resolve_canonical_artifact_path,
)
from realmheart_maintenance.version import RELEASE_VERSION
from realmheart_maintenance.forensics import (
    CapabilityObservation,
    capability_requires_success,
    capability_state_satisfied,
)

from .health import HealthCheckExecutor, HealthCheckResult, HealthStatus

_BUILD_FINGERPRINT_ARTIFACT = "core.build-fingerprints"


class ComponentHealth(str, Enum):
    HEALTHY = "healthy"
    DEGRADED = "degraded"
    FAILED = "failed"
    UNKNOWN = "unknown"


EXIT_CODES = {ComponentHealth.HEALTHY: 0, ComponentHealth.DEGRADED: 1,
              ComponentHealth.FAILED: 2, ComponentHealth.UNKNOWN: 3}
CRITICAL_CATEGORIES = frozenset({"core", "fx", "essential"})


@dataclass(frozen=True)
class BuildFingerprintComparison:
    """One build-time dependency version versus the current observation."""

    dependency_id: str
    build_version: str
    current_version: str | None
    drift: bool

    def to_dict(self) -> dict[str, object]:
        return {"dependency_id": self.dependency_id, "build_version": self.build_version,
                "current_version": self.current_version, "drift": self.drift}


@dataclass(frozen=True)
class ComponentDiagnosis:
    id: str
    name: str
    category: str
    status: ComponentHealth
    checks: tuple[HealthCheckResult, ...]
    uncertainties: tuple[str, ...] = ()
    build_fingerprints: tuple[BuildFingerprintComparison, ...] = ()

    def to_dict(self) -> dict[str, object]:
        return {"id": self.id, "name": self.name, "category": self.category,
                "status": self.status.value,
                "checks": [item.to_dict() for item in self.checks],
                "uncertainties": list(self.uncertainties),
                "build_fingerprints": [item.to_dict() for item in self.build_fingerprints]}


@dataclass(frozen=True)
class Diagnosis:
    release_version: str
    manifest_digest: str
    overall: ComponentHealth
    components: tuple[ComponentDiagnosis, ...]
    budget_exhausted: bool
    receipt: dict[str, object] | None = None

    def to_dict(self) -> dict[str, object]:
        return {"format_version": 1, "doctor_version": RELEASE_VERSION, "release_version": self.release_version,
                "manifest_digest": self.manifest_digest,
                "overall": self.overall.value,
                "budget_exhausted": self.budget_exhausted,
                "receipt": self.receipt,
                "components": [item.to_dict() for item in self.components]}


def diagnose(registry: ManifestRegistry, component: str | None = None, *,
             executor: HealthCheckExecutor | None = None,
             capability_prober=None, receipt=None,
             health_context: str = "doctor_manual",
             max_cost: str = "normal") -> Diagnosis:
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
        registry, context=health_context, max_cost=max_cost, check_ids=ids,
    )
    uncertainties_by_component: dict[str, list[str]] = {}
    if capability_prober is None:
        # Reuse the shared read-only capability prober instead of inventing a
        # second probe implementation; the seam keeps unit tests hermetic.
        from .acceptance import _probe_capability

        capability_prober = _probe_capability
    if receipt is not None:
        receipt_aligned = receipt.manifest_digest == registry.digest
        accepted = receipt.components.get(component) if component is not None else None
        if accepted is None and component is None:
            for key in registry.component_order:
                accepted_component = receipt.components.get(key)
                if accepted_component is not None and accepted_component.health in {"failed", "blocked"}:
                    uncertainties_by_component.setdefault(key, []).append("receipt_component_failed")
                elif accepted_component is not None and accepted_component.health in {"degraded", "warning", "unknown", "pending", "running", "skipped", "not_applicable", "pending_activation"}:
                    uncertainties_by_component.setdefault(key, []).append("receipt_component_uncertain")
        elif accepted is not None and accepted.health in {"failed", "blocked"}:
            uncertainties_by_component.setdefault(component, []).append("receipt_component_failed")
        elif accepted is not None and accepted.health in {"degraded", "warning", "unknown", "pending", "running", "skipped", "not_applicable", "pending_activation"}:
            uncertainties_by_component.setdefault(component, []).append("receipt_component_uncertain")
        if not receipt_aligned:
            for key in registry.component_order:
                uncertainties_by_component.setdefault(key, []).append("receipt_manifest_identity_drift")
    current_compositor_version: str | None = None
    if receipt is not None and receipt.fx is not None and receipt.fx.hyprland_version:
        hyprctl = registry.capabilities.get("runtime.hyprctl")
        if hyprctl is not None:
            observation = capability_prober(hyprctl, registry=registry)
            current_compositor_version = observation.version
            expected_version = ParsedVersion.parse(receipt.fx.hyprland_version)
            observed_version = ParsedVersion.parse(observation.version) if observation.version else None
            if expected_version is None or observed_version is None or expected_version != observed_version:
                # The FX plugin was built against a different compositor than
                # the one running now (or identity could not be established).
                # Never silently reuse a possibly ABI-incompatible plugin.
                uncertainties_by_component.setdefault("realmheart-fx", []).append("fx_build_compositor_drift")
    results: dict[str, ComponentDiagnosis] = {}
    observations: dict[str, CapabilityObservation] = {}
    for capability in registry.capabilities.values():
        if "runtime" not in capability.lifecycle:
            continue
        if capability.component_id is not None and capability.component_id not in selected:
            continue
        observations[capability.id] = capability_prober(capability, registry=registry)
    fingerprints_by_component = _build_fingerprint_evidence(registry, selected, observations, capability_prober)
    for key in registry.component_order:
        if key not in selected:
            continue
        spec = registry.components[key]
        checks = tuple(item for item in run.results if item.component_id == key)
        required = []
        optional = []
        uncertainties = []
        capability_failed = False
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
        for capability in registry.capabilities.values():
            if capability.component_id not in (None, key) or "runtime" not in capability.lifecycle:
                continue
            observation = observations.get(capability.id)
            if observation is None:
                observations[capability.id] = observation = capability_prober(capability, registry=registry)
            if capability_state_satisfied(registry, capability, observation.state):
                continue
            if observation.state == "not_applicable":
                uncertainties.append("runtime_capability_not_applicable")
                continue
            if observation.state == "unknown":
                uncertainties.append("runtime_capability_unknown")
                continue
            if capability_requires_success(registry, capability):
                uncertainties.append("required_runtime_capability_missing")
                capability_failed = True
                status = ComponentHealth.FAILED
            elif registry.components[capability.component_id or key].category in CRITICAL_CATEGORIES:
                uncertainties.append("required_runtime_capability_missing")
                capability_failed = True
                status = ComponentHealth.FAILED
            else:
                uncertainties.append("noncritical_runtime_capability_missing")
                status = ComponentHealth.DEGRADED
        upstream = [results[item.id].status for item in spec.realmheart_dependencies if item.required]
        failed_upstream = tuple(item.id for item in spec.realmheart_dependencies
                                if item.required and results[item.id].status is ComponentHealth.FAILED)
        component_uncertainties = uncertainties_by_component.get(key, [])
        uncertainties.extend(component_uncertainties)
        receipt_failed = "receipt_component_failed" in component_uncertainties
        if failed_upstream and not (receipt_failed or capability_failed or HealthStatus.FAIL in required):
            # The component itself has no observed failure: it inherits one from
            # a required dependency.  Say so instead of leaving an empty record.
            uncertainties.append("upstream_component_failed:" + ",".join(sorted(failed_upstream)))
        if receipt_failed or capability_failed or HealthStatus.FAIL in required or failed_upstream:
            status = ComponentHealth.FAILED
        elif (uncertainties or any(item in {HealthStatus.UNKNOWN, HealthStatus.NOT_APPLICABLE}
                                   for item in required) or ComponentHealth.UNKNOWN in upstream):
            status = ComponentHealth.UNKNOWN
        elif (any(item is not HealthStatus.PASS for item in optional)
              or HealthStatus.WARNING in required
              or ComponentHealth.DEGRADED in upstream):
            status = ComponentHealth.DEGRADED
        else:
            status = ComponentHealth.HEALTHY
        results[key] = ComponentDiagnosis(
            key, spec.name, spec.category, status, checks, tuple(uncertainties),
            fingerprints_by_component.get(key, ()),
        )
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
    return Diagnosis(
        registry.release_version,
        registry.digest,
        overall,
        values,
        run.budget_exhausted,
        receipt=_receipt_summary(receipt, registry, current_compositor_version),
    )


def _receipt_summary(receipt, registry, current_compositor_version: str | None) -> dict[str, object] | None:
    if receipt is None:
        return None
    provenance = receipt.build_provenance
    fx = receipt.fx
    return {
        "digest_matches": receipt.manifest_digest == registry.digest,
        "schema_version": receipt.schema_version,
        "installer_version": receipt.installer_version,
        "transaction_id": receipt.transaction_id,
        "install_health": receipt.install_health,
        "install_mode": receipt.install_mode,
        "installation_origin": receipt.installation_origin,
        "verified_at": receipt.verified_at,
        "build_provenance": None if provenance is None else {
            "source_revision": provenance.source_revision,
            "source_dirty": provenance.source_dirty,
            "cmake_version": provenance.cmake_version,
            "cmake_generator": provenance.cmake_generator,
            "cmake_build_type": provenance.cmake_build_type,
            "cmake_install_prefix": provenance.cmake_install_prefix,
            "cmake_source_dir": provenance.cmake_source_dir,
            "cmake_binary_dir": provenance.cmake_binary_dir,
            "cxx_compiler": provenance.cxx_compiler,
            "cxx_compiler_version": provenance.cxx_compiler_version,
            "hyprland_version": provenance.hyprland_version,
            "hyprland_commit": provenance.hyprland_commit,
            "fx_build_id": provenance.fx_build_id,
        },
        "fx": None if fx is None else {
            "required": fx.required,
            "compatibility": fx.compatibility,
            "build_id": fx.build_id,
            "hyprland_version": fx.hyprland_version,
            "hyprland_commit": fx.hyprland_commit,
            "hyprland_abi_hash": fx.hyprland_abi_hash,
            "current_compositor_version": current_compositor_version,
        },
    }


def render_diagnosis(result: Diagnosis, *, verbose: bool = False) -> str:
    lines = [f"Realmheart Doctor — {result.overall.value.upper()}"]
    for component in result.components:
        lines.append(f"  {component.status.value.upper():8} {component.id}")
        for item in component.checks:
            if verbose or item.status is not HealthStatus.PASS:
                lines.append(f"    {item.status.value}: {item.check_id} — {item.reason_code}")
        if verbose:
            for entry in component.build_fingerprints:
                if entry.drift:
                    lines.append(f"    build-drift: {entry.dependency_id} built {entry.build_version} now {entry.current_version}")
        lines.extend(f"    unknown: {reason}" for reason in component.uncertainties)
    return "\n".join(lines)


def _normalised_version(value: str | None) -> str | None:
    if not value:
        return None
    parsed = ParsedVersion.parse(value)
    if parsed is None:
        return None
    return f"{parsed.major}.{parsed.minor}.{parsed.patch}"


def _load_component_fingerprint(directory: str, component_id: str) -> dict[str, str] | None:
    """Read one component's build fingerprint, tolerating absence or corruption."""

    path = Path(directory) / f"{component_id}.json"
    try:
        raw = read_regular_file(path, max_bytes=MAX_FINGERPRINT_BYTES)
    except (FingerprintLimitExceeded, FingerprintObservationError, OSError, ValueError):
        return None
    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError):
        return None
    if not isinstance(payload, dict) or payload.get("format_version") != 1:
        return None
    dependencies = payload.get("dependencies")
    if not isinstance(dependencies, dict):
        return None
    entries = {
        key: value
        for key, value in dependencies.items()
        if isinstance(key, str) and key and isinstance(value, str) and value
    }
    return entries or None


def _current_dependency_version(
    registry: ManifestRegistry,
    dependency_id: str,
    observations: dict[str, CapabilityObservation],
    capability_prober,
) -> str | None:
    for capability in registry.capabilities.values():
        if capability.dependency_id != dependency_id:
            continue
        observation = observations.get(capability.id)
        if observation is None and capability_prober is not None:
            # Build-lifecycle capabilities are not part of the runtime loop;
            # observe them on demand for the specific fingerprint comparison.
            observation = capability_prober(capability, registry=registry)
            observations[capability.id] = observation
        if observation is None:
            continue
        normalised = _normalised_version(observation.version)
        if normalised is not None:
            return normalised
    return None


def _build_fingerprint_evidence(
    registry: ManifestRegistry,
    selected: set[str],
    observations: dict[str, CapabilityObservation],
    capability_prober=None,
) -> dict[str, tuple[BuildFingerprintComparison, ...]]:
    """Compare declared build-time versions against current observations.

    Missing metadata is not an error: an older build simply produces no
    comparison evidence, never a fabricated match.
    """

    artifact = registry.artifacts.get(_BUILD_FINGERPRINT_ARTIFACT)
    if artifact is None:
        return {}
    directory = resolve_canonical_artifact_path(artifact.path)
    if not directory:
        return {}
    evidence: dict[str, tuple[BuildFingerprintComparison, ...]] = {}
    for component_id in registry.component_order:
        if component_id not in selected:
            continue
        fingerprint = _load_component_fingerprint(directory, component_id)
        if not fingerprint:
            continue
        comparisons = []
        for dependency_id in sorted(fingerprint):
            build_version = _normalised_version(fingerprint[dependency_id])
            if build_version is None:
                continue
            current_version = _current_dependency_version(registry, dependency_id, observations, capability_prober)
            comparisons.append(BuildFingerprintComparison(
                dependency_id, build_version, current_version,
                drift=current_version is not None and current_version != build_version,
            ))
        if comparisons:
            evidence[component_id] = tuple(comparisons)
    return evidence
