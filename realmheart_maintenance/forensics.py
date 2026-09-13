"""Tool-independent installed-state forensics for Realmheart maintenance consumers.

This module is intentionally safe for Doctor-style consumers.  It understands
only the canonical manifest, the durable installed-state receipt, and a
read-only current-health snapshot.  It imports no installer handlers, mutation
engines, package adapters, or recovery code.
"""
from __future__ import annotations

import json
import re
from dataclasses import asdict, dataclass
from enum import Enum
from pathlib import Path
from types import MappingProxyType
from typing import Any, Mapping, Sequence

from .manifest import ManifestRegistry, VersionCompatibility, classify_version

SUPPORTED_RECEIPT_SCHEMA = 2
SUPPORTED_HEALTH_SNAPSHOT_SCHEMA = 1
_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
_SATISFIED_CAPABILITY_STATES = {"pass", "not_applicable"}
_RECEIPT_CAPABILITY_STATES = {"pass", "missing", "failed", "not_applicable"}
_SNAPSHOT_CAPABILITY_STATES = _RECEIPT_CAPABILITY_STATES | {"unknown"}
_REQUIREMENTS = {"required", "component", "soft"}
_LIFECYCLES = {"build", "install", "runtime", "verification", "repair", "ordering"}
_INSTALL_HEALTH = {"healthy", "degraded", "failed"}
_ACTIVATION_STATES = {"active", "pending_session_restart", "unknown"}
_SNAPSHOT_ACTIVATION_STATES = _ACTIVATION_STATES | {"failed"}
_RUNTIME_HEALTH = {"healthy", "degraded", "failed", "unknown"}
_COST_RANK = {"cheap": 0, "normal": 1, "expensive": 2}
_SEVERITY_RANK = {"info": 0, "warning": 1, "error": 2, "critical": 3}
_REPAIR_LIFECYCLES = {"build", "install", "verification", "repair"}


class ForensicContractError(ValueError):
    """Raised when persisted forensic input cannot be trusted or understood."""


class DriftKind(str, Enum):
    MANIFEST = "manifest"
    DEPENDENCY = "dependency"
    ARTIFACT = "artifact"
    RUNTIME = "runtime"


class ReadinessState(str, Enum):
    HEALTHY = "healthy"
    DEGRADED = "degraded"
    FAILED = "failed"
    UNKNOWN = "unknown"


@dataclass(frozen=True)
class ReceiptComponent:
    component_id: str
    display_name: str
    category: str
    health: str
    blocked_by: tuple[str, ...]
    artifact_ids: tuple[str, ...]
    build_unit_ids: tuple[str, ...]


@dataclass(frozen=True)
class ReceiptCapability:
    capability_id: str
    component_id: str | None
    requirement: str
    lifecycle: tuple[str, ...]
    state: str
    version: str | None


@dataclass(frozen=True)
class ReceiptArtifact:
    artifact_id: str
    component_id: str
    path: str
    artifact_type: str
    ownership: str
    mode: str | None
    sha256: str | None
    immutable_fingerprint: str | None


@dataclass(frozen=True)
class InstalledStateReceipt:
    schema_version: int
    realmheart_version: str
    manifest_schema_version: int
    manifest_digest: str
    installer_version: str
    transaction_id: str
    disposition: str
    install_health: str
    activation_state: str
    runtime_health: str
    components: Mapping[str, ReceiptComponent]
    capabilities: Mapping[str, ReceiptCapability]
    artifacts: Mapping[str, ReceiptArtifact]


@dataclass(frozen=True)
class CapabilityObservation:
    capability_id: str
    state: str
    version: str | None = None
    detail: str | None = None


@dataclass(frozen=True)
class ArtifactObservation:
    artifact_id: str
    exists: bool
    sha256: str | None = None
    immutable_fingerprint: str | None = None
    mode: str | None = None


@dataclass(frozen=True)
class CurrentHealthSnapshot:
    schema_version: int
    captured_at: str
    activation_state: str
    runtime_health: str
    capabilities: Mapping[str, CapabilityObservation]
    artifacts: Mapping[str, ArtifactObservation]


@dataclass(frozen=True)
class DriftRecord:
    drift_id: str
    kind: DriftKind
    error_code: str
    severity: str
    subject_id: str
    component_id: str | None
    dependency_id: str | None
    previous: str | None
    current: str | None
    affects_runtime: bool
    affects_repair: bool
    summary: str


@dataclass(frozen=True)
class ForensicIncident:
    incident_id: str
    root_kind: str
    root_id: str
    error_code: str
    severity: str
    drift_ids: tuple[str, ...]
    capability_ids: tuple[str, ...]
    affected_components: tuple[str, ...]
    summary: str


@dataclass(frozen=True)
class ForensicReport:
    schema_version: int
    realmheart_version: str
    manifest_digest: str
    receipt_manifest_digest: str
    receipt_transaction_id: str
    snapshot_captured_at: str
    runtime_health: str
    repair_readiness: ReadinessState
    selected_health_check_ids: tuple[str, ...]
    drifts: tuple[DriftRecord, ...]
    incidents: tuple[ForensicIncident, ...]

    @property
    def has_drift(self) -> bool:
        return bool(self.drifts)

    def to_dict(self) -> dict[str, object]:
        payload = asdict(self)
        payload["repair_readiness"] = self.repair_readiness.value
        for item in payload["drifts"]:
            item["kind"] = item["kind"].value if isinstance(item["kind"], DriftKind) else item["kind"]
        return payload


def _expect_mapping(value: Any, field: str) -> Mapping[str, Any]:
    if not isinstance(value, dict):
        raise ForensicContractError(f"{field} must be an object")
    return value


def _expect_string(value: Any, field: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str) or (not allow_empty and not value):
        raise ForensicContractError(f"{field} must be a non-empty string")
    return value


def _optional_string(value: Any, field: str) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str):
        raise ForensicContractError(f"{field} must be a string or null")
    return value


def _string_tuple(value: Any, field: str) -> tuple[str, ...]:
    if value is None:
        return ()
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise ForensicContractError(f"{field} must be an array of strings")
    return tuple(value)


def _expect_id(value: Any, field: str) -> str:
    text = _expect_string(value, field)
    if not _ID_RE.fullmatch(text):
        raise ForensicContractError(f"{field} contains an invalid id: {text!r}")
    return text


def _load_json_file(path: Path, *, label: str) -> Mapping[str, Any]:
    path = Path(path)
    if path.is_symlink():
        raise ForensicContractError(f"{label} must not be a symlink: {path}")
    try:
        if not path.is_file():
            raise ForensicContractError(f"{label} is not a regular file: {path}")
        payload = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise ForensicContractError(f"cannot read {label}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise ForensicContractError(f"cannot parse {label}: {exc}") from exc
    return _expect_mapping(payload, label)


def parse_installed_receipt(payload: Mapping[str, Any]) -> InstalledStateReceipt:
    schema = payload.get("schema_version")
    if not isinstance(schema, int):
        raise ForensicContractError("installed-state schema_version must be an integer")
    if schema > SUPPORTED_RECEIPT_SCHEMA:
        raise ForensicContractError(
            f"installed-state schema {schema} is newer than supported schema {SUPPORTED_RECEIPT_SCHEMA}"
        )
    if schema != SUPPORTED_RECEIPT_SCHEMA:
        raise ForensicContractError(f"unsupported installed-state schema {schema}")

    disposition = _expect_string(payload.get("disposition"), "installed-state disposition")
    if disposition != "kept":
        raise ForensicContractError(f"installed-state disposition must be kept, got {disposition!r}")

    components_raw = _expect_mapping(payload.get("components"), "installed-state components")
    dependencies_raw = _expect_mapping(payload.get("dependencies"), "installed-state dependencies")
    artifacts_raw = _expect_mapping(payload.get("artifacts"), "installed-state artifacts")

    components: dict[str, ReceiptComponent] = {}
    for raw_id, value in components_raw.items():
        cid = _expect_id(raw_id, "installed-state component id")
        item = _expect_mapping(value, f"installed-state component {cid}")
        components[cid] = ReceiptComponent(
            component_id=cid,
            display_name=_expect_string(item.get("display_name", cid), f"component {cid} display_name"),
            category=_expect_string(item.get("category", "unknown"), f"component {cid} category"),
            health=_expect_string(item.get("health"), f"component {cid} health"),
            blocked_by=_string_tuple(item.get("blocked_by", []), f"component {cid} blocked_by"),
            artifact_ids=_string_tuple(item.get("artifact_ids", []), f"component {cid} artifact_ids"),
            build_unit_ids=_string_tuple(item.get("build_unit_ids", []), f"component {cid} build_unit_ids"),
        )

    capabilities: dict[str, ReceiptCapability] = {}
    for raw_id, value in dependencies_raw.items():
        capid = _expect_id(raw_id, "installed-state capability id")
        item = _expect_mapping(value, f"installed-state capability {capid}")
        component_id = item.get("component_id")
        if component_id is not None:
            component_id = _expect_id(component_id, f"capability {capid} component_id")
        requirement = _expect_string(item.get("requirement"), f"capability {capid} requirement")
        lifecycle = _string_tuple(item.get("lifecycle", []), f"capability {capid} lifecycle")
        state = _expect_string(item.get("state"), f"capability {capid} state")
        if requirement not in _REQUIREMENTS:
            raise ForensicContractError(f"capability {capid} has invalid requirement {requirement!r}")
        if not lifecycle or any(value not in _LIFECYCLES for value in lifecycle):
            raise ForensicContractError(f"capability {capid} has invalid lifecycle {lifecycle!r}")
        if state not in _RECEIPT_CAPABILITY_STATES:
            raise ForensicContractError(f"capability {capid} has invalid state {state!r}")
        capabilities[capid] = ReceiptCapability(
            capability_id=capid,
            component_id=component_id,
            requirement=requirement,
            lifecycle=lifecycle,
            state=state,
            version=_optional_string(item.get("version"), f"capability {capid} version"),
        )

    artifacts: dict[str, ReceiptArtifact] = {}
    for raw_id, value in artifacts_raw.items():
        aid = _expect_id(raw_id, "installed-state artifact id")
        item = _expect_mapping(value, f"installed-state artifact {aid}")
        artifacts[aid] = ReceiptArtifact(
            artifact_id=aid,
            component_id=_expect_id(item.get("component_id"), f"artifact {aid} component_id"),
            path=_expect_string(item.get("path"), f"artifact {aid} path"),
            artifact_type=_expect_string(item.get("type"), f"artifact {aid} type"),
            ownership=_expect_string(item.get("ownership"), f"artifact {aid} ownership"),
            mode=_optional_string(item.get("mode"), f"artifact {aid} mode"),
            sha256=_optional_string(item.get("sha256"), f"artifact {aid} sha256"),
            immutable_fingerprint=_optional_string(
                item.get("immutable_fingerprint"), f"artifact {aid} immutable_fingerprint"
            ),
        )

    manifest_schema = payload.get("manifest_schema_version")
    if not isinstance(manifest_schema, int):
        raise ForensicContractError("installed-state manifest_schema_version must be an integer")

    install_health = _expect_string(payload.get("install_health"), "installed-state install_health")
    activation_state = _expect_string(payload.get("activation_state"), "installed-state activation_state")
    runtime_health = _expect_string(payload.get("runtime_health"), "installed-state runtime_health")
    if install_health not in _INSTALL_HEALTH:
        raise ForensicContractError(f"installed-state has invalid install_health {install_health!r}")
    if activation_state not in _ACTIVATION_STATES:
        raise ForensicContractError(f"installed-state has invalid activation_state {activation_state!r}")
    if runtime_health not in _RUNTIME_HEALTH:
        raise ForensicContractError(f"installed-state has invalid runtime_health {runtime_health!r}")

    return InstalledStateReceipt(
        schema_version=schema,
        realmheart_version=_expect_string(payload.get("realmheart_version"), "installed-state realmheart_version"),
        manifest_schema_version=manifest_schema,
        manifest_digest=_expect_string(payload.get("manifest_set_sha256"), "installed-state manifest_set_sha256"),
        installer_version=_expect_string(payload.get("installer_version"), "installed-state installer_version"),
        transaction_id=_expect_string(payload.get("transaction_id"), "installed-state transaction_id"),
        disposition=disposition,
        install_health=install_health,
        activation_state=activation_state,
        runtime_health=runtime_health,
        components=MappingProxyType(components),
        capabilities=MappingProxyType(capabilities),
        artifacts=MappingProxyType(artifacts),
    )


def load_installed_receipt(path: Path) -> InstalledStateReceipt:
    return parse_installed_receipt(_load_json_file(path, label="installed-state receipt"))


def parse_health_snapshot(payload: Mapping[str, Any]) -> CurrentHealthSnapshot:
    schema = payload.get("schema_version")
    if not isinstance(schema, int):
        raise ForensicContractError("health snapshot schema_version must be an integer")
    if schema > SUPPORTED_HEALTH_SNAPSHOT_SCHEMA:
        raise ForensicContractError(
            f"health snapshot schema {schema} is newer than supported schema {SUPPORTED_HEALTH_SNAPSHOT_SCHEMA}"
        )
    if schema != SUPPORTED_HEALTH_SNAPSHOT_SCHEMA:
        raise ForensicContractError(f"unsupported health snapshot schema {schema}")

    capabilities_raw = _expect_mapping(payload.get("capabilities", {}), "health snapshot capabilities")
    artifacts_raw = _expect_mapping(payload.get("artifacts", {}), "health snapshot artifacts")
    capabilities: dict[str, CapabilityObservation] = {}
    artifacts: dict[str, ArtifactObservation] = {}

    for raw_id, value in capabilities_raw.items():
        capid = _expect_id(raw_id, "health snapshot capability id")
        item = _expect_mapping(value, f"health snapshot capability {capid}")
        state = _expect_string(item.get("state"), f"health snapshot capability {capid} state")
        if state not in _SNAPSHOT_CAPABILITY_STATES:
            raise ForensicContractError(f"health snapshot capability {capid} has invalid state {state!r}")
        capabilities[capid] = CapabilityObservation(
            capability_id=capid,
            state=state,
            version=_optional_string(item.get("version"), f"health snapshot capability {capid} version"),
            detail=_optional_string(item.get("detail"), f"health snapshot capability {capid} detail"),
        )

    for raw_id, value in artifacts_raw.items():
        aid = _expect_id(raw_id, "health snapshot artifact id")
        item = _expect_mapping(value, f"health snapshot artifact {aid}")
        exists = item.get("exists")
        if not isinstance(exists, bool):
            raise ForensicContractError(f"health snapshot artifact {aid} exists must be boolean")
        artifacts[aid] = ArtifactObservation(
            artifact_id=aid,
            exists=exists,
            sha256=_optional_string(item.get("sha256"), f"health snapshot artifact {aid} sha256"),
            immutable_fingerprint=_optional_string(
                item.get("immutable_fingerprint"), f"health snapshot artifact {aid} immutable_fingerprint"
            ),
            mode=_optional_string(item.get("mode"), f"health snapshot artifact {aid} mode"),
        )

    activation_state = _expect_string(payload.get("activation_state"), "health snapshot activation_state")
    runtime_health = _expect_string(payload.get("runtime_health"), "health snapshot runtime_health")
    if activation_state not in _SNAPSHOT_ACTIVATION_STATES:
        raise ForensicContractError(f"health snapshot has invalid activation_state {activation_state!r}")
    if runtime_health not in _RUNTIME_HEALTH:
        raise ForensicContractError(f"health snapshot has invalid runtime_health {runtime_health!r}")

    return CurrentHealthSnapshot(
        schema_version=schema,
        captured_at=_expect_string(payload.get("captured_at"), "health snapshot captured_at"),
        activation_state=activation_state,
        runtime_health=runtime_health,
        capabilities=MappingProxyType(capabilities),
        artifacts=MappingProxyType(artifacts),
    )


def load_health_snapshot(path: Path) -> CurrentHealthSnapshot:
    return parse_health_snapshot(_load_json_file(path, label="current health snapshot"))


def select_health_checks(
    registry: ManifestRegistry,
    *,
    context: str,
    max_cost: str = "cheap",
    allowed_side_effects: Sequence[str] = ("none", "read_only"),
) -> tuple[str, ...]:
    """Return canonical health-check IDs allowed for one Doctor execution context."""

    if max_cost not in _COST_RANK:
        raise ForensicContractError(f"unknown health-check cost {max_cost!r}")
    allowed = set(allowed_side_effects)
    selected: list[str] = []
    for check in registry.health_checks.values():
        if context not in check.contexts:
            continue
        if _COST_RANK[check.cost] > _COST_RANK[max_cost]:
            continue
        if check.side_effects not in allowed:
            continue
        selected.append(check.id)
    return tuple(selected)


def _severity_max(values: Sequence[str]) -> str:
    return max(values, key=lambda item: _SEVERITY_RANK[item], default="info")


def _dependent_components(registry: ManifestRegistry, roots: set[str]) -> set[str]:
    affected = set(roots)
    changed = True
    while changed:
        changed = False
        for component in registry.components.values():
            if component.id in affected:
                continue
            if any(dep.required and dep.id in affected for dep in component.realmheart_dependencies):
                affected.add(component.id)
                changed = True
    return affected


def _repair_readiness(registry: ManifestRegistry, snapshot: CurrentHealthSnapshot) -> ReadinessState:
    relevant = [cap for cap in registry.capabilities.values() if _REPAIR_LIFECYCLES.intersection(cap.lifecycle)]
    if not relevant:
        return ReadinessState.HEALTHY
    observed = 0
    worst = ReadinessState.HEALTHY
    for cap in relevant:
        current = snapshot.capabilities.get(cap.id)
        if current is None:
            continue
        observed += 1
        if current.state in _SATISFIED_CAPABILITY_STATES:
            continue
        if cap.requirement == "required":
            worst = ReadinessState.FAILED
        elif worst is not ReadinessState.FAILED:
            worst = ReadinessState.DEGRADED
    if worst is not ReadinessState.HEALTHY:
        return worst
    if observed != len(relevant):
        return ReadinessState.UNKNOWN
    return ReadinessState.HEALTHY


def analyze_forensics(
    registry: ManifestRegistry,
    receipt: InstalledStateReceipt,
    snapshot: CurrentHealthSnapshot,
    *,
    health_context: str = "doctor_background",
    max_health_cost: str = "cheap",
) -> ForensicReport:
    """Perform a manifest ↔ accepted receipt ↔ current-state comparison."""

    drifts: list[DriftRecord] = []

    def add(
        kind: DriftKind,
        code: str,
        severity: str,
        subject_id: str,
        *,
        component_id: str | None = None,
        dependency_id: str | None = None,
        previous: str | None = None,
        current: str | None = None,
        affects_runtime: bool = False,
        affects_repair: bool = False,
        summary: str,
    ) -> None:
        drifts.append(
            DriftRecord(
                drift_id=f"drift-{len(drifts) + 1:03d}",
                kind=kind,
                error_code=code,
                severity=severity,
                subject_id=subject_id,
                component_id=component_id,
                dependency_id=dependency_id,
                previous=previous,
                current=current,
                affects_runtime=affects_runtime,
                affects_repair=affects_repair,
                summary=summary,
            )
        )

    if receipt.manifest_digest != registry.digest or receipt.manifest_schema_version != registry.schema_version:
        add(
            DriftKind.MANIFEST,
            "RH_FORENSIC_MANIFEST_IDENTITY_DRIFT",
            "warning",
            "canonical-manifest",
            previous=receipt.manifest_digest,
            current=registry.digest,
            affects_repair=True,
            summary="current canonical manifest differs from the manifest accepted by the installed receipt",
        )

    for capid, accepted in receipt.capabilities.items():
        current = snapshot.capabilities.get(capid)
        manifest_cap = registry.capabilities.get(capid)
        if current is None or manifest_cap is None:
            continue
        dependency_id = manifest_cap.dependency_id
        runtime = "runtime" in manifest_cap.lifecycle
        repair = bool(_REPAIR_LIFECYCLES.intersection(manifest_cap.lifecycle))
        was_ok = accepted.state in _SATISFIED_CAPABILITY_STATES
        is_ok = current.state in _SATISFIED_CAPABILITY_STATES
        if was_ok and not is_ok:
            severity = "critical" if manifest_cap.requirement == "required" and runtime else "error" if manifest_cap.requirement != "soft" else "warning"
            code = "RH_FORENSIC_DEPENDENCY_MISSING" if current.state == "missing" else "RH_FORENSIC_DEPENDENCY_FAILED"
            add(
                DriftKind.DEPENDENCY,
                code,
                severity,
                capid,
                component_id=manifest_cap.component_id,
                dependency_id=dependency_id,
                previous=accepted.state,
                current=current.state,
                affects_runtime=runtime,
                affects_repair=repair,
                summary=f"capability {capid} regressed from {accepted.state} to {current.state}",
            )
        elif accepted.state != current.state:
            add(
                DriftKind.DEPENDENCY,
                "RH_FORENSIC_DEPENDENCY_STATE_DRIFT",
                "warning",
                capid,
                component_id=manifest_cap.component_id,
                dependency_id=dependency_id,
                previous=accepted.state,
                current=current.state,
                affects_runtime=runtime,
                affects_repair=repair,
                summary=f"capability {capid} state changed from {accepted.state} to {current.state}",
            )

        if accepted.version and current.version and accepted.version != current.version:
            dep_spec = registry.dependencies.get(dependency_id)
            incompatible = False
            if dep_spec is not None:
                compatibility = classify_version(dep_spec.version, current.version)
                incompatible = compatibility is VersionCompatibility.INCOMPATIBLE
            add(
                DriftKind.DEPENDENCY,
                "RH_FORENSIC_DEPENDENCY_VERSION_INCOMPATIBLE" if incompatible else "RH_FORENSIC_DEPENDENCY_VERSION_DRIFT",
                "error" if incompatible else "warning",
                capid,
                component_id=manifest_cap.component_id,
                dependency_id=dependency_id,
                previous=accepted.version,
                current=current.version,
                affects_runtime=runtime,
                affects_repair=repair,
                summary=f"capability {capid} version changed from {accepted.version} to {current.version}",
            )

    for aid, accepted in receipt.artifacts.items():
        current = snapshot.artifacts.get(aid)
        manifest_artifact = registry.artifacts.get(aid)
        if current is None or manifest_artifact is None:
            continue
        if not current.exists:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_MISSING",
                "critical" if manifest_artifact.required and registry.components[manifest_artifact.component_id].category in {"core", "fx"} else "error",
                aid,
                component_id=manifest_artifact.component_id,
                previous="present",
                current="missing",
                affects_runtime=True,
                summary=f"artifact {aid} recorded by the installed receipt is now missing",
            )
            continue
        if accepted.sha256 and current.sha256 and accepted.sha256 != current.sha256:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_HASH_DRIFT",
                "error",
                aid,
                component_id=manifest_artifact.component_id,
                previous=accepted.sha256,
                current=current.sha256,
                affects_runtime=True,
                summary=f"immutable artifact {aid} no longer matches its accepted SHA-256",
            )
        if (
            accepted.immutable_fingerprint
            and current.immutable_fingerprint
            and accepted.immutable_fingerprint != current.immutable_fingerprint
        ):
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_FINGERPRINT_DRIFT",
                "error",
                aid,
                component_id=manifest_artifact.component_id,
                previous=accepted.immutable_fingerprint,
                current=current.immutable_fingerprint,
                affects_runtime=True,
                summary=f"immutable artifact {aid} no longer matches its accepted fingerprint",
            )
        if accepted.mode and current.mode and accepted.mode != current.mode:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_MODE_DRIFT",
                "error" if manifest_artifact.required else "warning",
                aid,
                component_id=manifest_artifact.component_id,
                previous=accepted.mode,
                current=current.mode,
                affects_runtime=True,
                summary=f"artifact {aid} mode changed from {accepted.mode} to {current.mode}",
            )

    if receipt.activation_state != snapshot.activation_state:
        add(
            DriftKind.RUNTIME,
            "RH_FORENSIC_ACTIVATION_DRIFT",
            "warning" if snapshot.activation_state != "failed" else "error",
            "runtime.activation",
            previous=receipt.activation_state,
            current=snapshot.activation_state,
            affects_runtime=True,
            summary=f"activation state changed from {receipt.activation_state} to {snapshot.activation_state}",
        )
    if receipt.runtime_health != snapshot.runtime_health:
        add(
            DriftKind.RUNTIME,
            "RH_FORENSIC_RUNTIME_HEALTH_DRIFT",
            "critical" if snapshot.runtime_health == "failed" else "warning",
            "runtime.health",
            previous=receipt.runtime_health,
            current=snapshot.runtime_health,
            affects_runtime=True,
            summary=f"runtime health changed from {receipt.runtime_health} to {snapshot.runtime_health}",
        )

    incidents: list[ForensicIncident] = []
    dependency_groups: dict[str, list[DriftRecord]] = {}
    for drift in drifts:
        if drift.kind is DriftKind.DEPENDENCY and drift.dependency_id:
            dependency_groups.setdefault(drift.dependency_id, []).append(drift)
    for dependency_id in sorted(dependency_groups):
        items = dependency_groups[dependency_id]
        direct = {item.component_id for item in items if item.component_id}
        affected = _dependent_components(registry, direct)
        incidents.append(
            ForensicIncident(
                incident_id=f"incident-{len(incidents) + 1:03d}",
                root_kind="dependency",
                root_id=dependency_id,
                error_code="RH_FORENSIC_DEPENDENCY_DRIFT",
                severity=_severity_max([item.severity for item in items]),
                drift_ids=tuple(item.drift_id for item in items),
                capability_ids=tuple(sorted({item.subject_id for item in items})),
                affected_components=tuple(sorted(affected)),
                summary=f"dependency {dependency_id} drift affects {len(affected)} component(s)",
            )
        )

    artifact_by_component: dict[str, list[DriftRecord]] = {}
    for drift in drifts:
        if drift.kind is DriftKind.ARTIFACT and drift.component_id:
            artifact_by_component.setdefault(drift.component_id, []).append(drift)
    for component_id in sorted(artifact_by_component):
        items = artifact_by_component[component_id]
        incidents.append(
            ForensicIncident(
                incident_id=f"incident-{len(incidents) + 1:03d}",
                root_kind="artifact",
                root_id=component_id,
                error_code="RH_FORENSIC_ARTIFACT_DRIFT",
                severity=_severity_max([item.severity for item in items]),
                drift_ids=tuple(item.drift_id for item in items),
                capability_ids=(),
                affected_components=tuple(sorted(_dependent_components(registry, {component_id}))),
                summary=f"artifact drift rooted in component {component_id}",
            )
        )

    return ForensicReport(
        schema_version=1,
        realmheart_version=receipt.realmheart_version,
        manifest_digest=registry.digest,
        receipt_manifest_digest=receipt.manifest_digest,
        receipt_transaction_id=receipt.transaction_id,
        snapshot_captured_at=snapshot.captured_at,
        runtime_health=snapshot.runtime_health,
        repair_readiness=_repair_readiness(registry, snapshot),
        selected_health_check_ids=select_health_checks(
            registry, context=health_context, max_cost=max_health_cost
        ),
        drifts=tuple(drifts),
        incidents=tuple(incidents),
    )
