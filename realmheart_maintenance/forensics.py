"""Tool-independent installed-state forensics for Realmheart maintenance consumers.

This module is intentionally safe for Doctor-style consumers.  It understands
only the canonical manifest, the durable installed-state receipt, and a
read-only current-health snapshot.  It imports no installer handlers, mutation
engines, package adapters, or recovery code.
"""
from __future__ import annotations

import json
import re
import stat
from dataclasses import asdict, dataclass
from enum import Enum
from pathlib import Path
from types import MappingProxyType
from typing import Any, Mapping, Sequence

from .fingerprint import FingerprintLimitExceeded, read_regular_file
from .manifest import (
    ManifestRegistry,
    ParsedVersion,
    VersionCompatibility,
    VersionSpec,
    classify_version,
    normalize_observed_artifact_path,
    resolve_canonical_artifact_path,
)

SUPPORTED_RECEIPT_SCHEMA = 2
SUPPORTED_HEALTH_SNAPSHOT_SCHEMA = 2
LEGACY_HEALTH_SNAPSHOT_SCHEMA = 1
_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
_RECEIPT_CAPABILITY_STATES = {"pass", "missing", "failed", "not_applicable"}
_SNAPSHOT_CAPABILITY_STATES = _RECEIPT_CAPABILITY_STATES | {"unknown"}
_REQUIREMENTS = {"required", "component", "soft"}
_LIFECYCLES = {"build", "install", "runtime", "verification", "repair", "ordering"}
_COMPONENT_HEALTH = {
    "healthy",
    "degraded",
    "failed",
    "blocked",
    "not_applicable",
    "pending_activation",
}
_INSTALL_HEALTH = {"healthy", "degraded", "failed"}
_ACTIVATION_STATES = {"active", "pending_session_restart", "unknown"}
_SNAPSHOT_ACTIVATION_STATES = _ACTIVATION_STATES | {"failed"}
_RUNTIME_HEALTH = {"healthy", "degraded", "failed", "unknown"}
_FILESYSTEM_TYPES = {"file", "directory", "symlink", "other"}
_IMMUTABLE_OWNERSHIPS = {"release", "system"}
_SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
_VERSION_TOKEN_RE = re.compile(r"(?<![A-Za-z0-9])v?\d+\.\d+(?:\.\d+)?(?:[-+][0-9A-Za-z.-]+)?(?![A-Za-z0-9])")
MAX_FORENSIC_JSON_BYTES = 4 * 1024 * 1024
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


class ObservationOutcome(str, Enum):
    OBSERVED = "observed"
    MISSING = "missing"
    UNKNOWN = "unknown"
    LIMIT_EXCEEDED = "limit_exceeded"


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
    error: str | None = None
    filesystem_type: str | None = None
    outcome: ObservationOutcome | None = None
    path: str | None = None


@dataclass(frozen=True)
class CurrentHealthSnapshot:
    schema_version: int
    captured_at: str
    activation_state: str
    runtime_health: str
    capabilities: Mapping[str, CapabilityObservation]
    artifacts: Mapping[str, ArtifactObservation]


def serialize_health_snapshot(snapshot: CurrentHealthSnapshot) -> dict[str, object]:
    """Return the JSON contract for a current-health snapshot.

    The path is serialized even when ``None`` so a legacy or incomplete
    in-memory observation cannot be mistaken for a path-bound observation by a
    downstream consumer.
    """

    capabilities = {
        capability_id: {
            "state": observation.state,
            "version": observation.version,
            "detail": observation.detail,
        }
        for capability_id, observation in snapshot.capabilities.items()
    }
    artifacts = {
        artifact_id: {
            "path": observation.path,
            "exists": observation.exists,
            "sha256": observation.sha256,
            "immutable_fingerprint": observation.immutable_fingerprint,
            "mode": observation.mode,
            "error": observation.error,
            "filesystem_type": observation.filesystem_type,
            "outcome": (
                observation.outcome.value
                if isinstance(observation.outcome, ObservationOutcome)
                else observation.outcome
            ),
        }
        for artifact_id, observation in snapshot.artifacts.items()
    }
    return {
        "schema_version": snapshot.schema_version,
        "captured_at": snapshot.captured_at,
        "activation_state": snapshot.activation_state,
        "runtime_health": snapshot.runtime_health,
        "capabilities": capabilities,
        "artifacts": artifacts,
    }


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


def _optional_digest(value: Any, field: str) -> str | None:
    text = _optional_string(value, field)
    if text is not None and not _SHA256_RE.fullmatch(text):
        raise ForensicContractError(f"{field} must be a 64-character hexadecimal SHA-256 digest or null")
    return text


def _observation_outcome(value: Any) -> ObservationOutcome | None:
    if isinstance(value, ObservationOutcome):
        return value
    try:
        return ObservationOutcome(value)
    except (TypeError, ValueError):
        return None


def _valid_mode(value: Any) -> bool:
    return value is None or (
        isinstance(value, str)
        and len(value) == 4
        and all(character in "01234567" for character in value)
    )


def _valid_digest(value: Any) -> bool:
    return value is None or (isinstance(value, str) and _SHA256_RE.fullmatch(value) is not None)


def _valid_observed_path(value: Any) -> bool:
    return normalize_observed_artifact_path(value) is not None


def canonical_filesystem_type(artifact_type: str) -> str:
    """Return the no-follow filesystem type required by an artifact kind."""

    return "directory" if artifact_type == "directory" else "file"


def artifact_integrity_fields(artifact_spec) -> frozenset[str]:
    """Return receipt fields required to establish a canonical artifact.

    Release/system artifacts are immutable receipt identities and therefore
    require a fingerprint (and a content hash for regular-file artifacts).
    Every required or managed artifact still requires its observed mode.  User
    and shared artifacts intentionally do not require content hashes because
    the installer treats their contents as mutable state.
    """

    fields: set[str] = set()
    if artifact_spec.required or artifact_spec.managed or artifact_spec.mode is not None:
        fields.add("mode")
    if artifact_spec.ownership in _IMMUTABLE_OWNERSHIPS:
        if artifact_spec.required or artifact_spec.managed:
            fields.add("immutable_fingerprint")
            if canonical_filesystem_type(artifact_spec.type) == "file":
                fields.add("sha256")
    return frozenset(fields)


def capability_version_required(registry: ManifestRegistry, capability_spec) -> bool:
    """Whether a capability's canonical contract requires version evidence."""

    dependency = registry.dependencies.get(capability_spec.dependency_id)
    args = capability_spec.probe.args
    if args.get("version_argv"):
        return True
    if any(args.get(field) for field in ("minimum_version", "maximum_version", "exact_version", "tested_ranges", "known_incompatible")):
        return True
    if dependency is None:
        return False
    version = dependency.version
    return bool(
        version.minimum_version
        or version.maximum_version
        or version.exact_version
        or version.tested_ranges
        or version.known_incompatible
    )


def _capability_version_specs(
    registry: ManifestRegistry,
    capability_spec,
) -> tuple[VersionSpec, ...]:
    dependency = registry.dependencies.get(capability_spec.dependency_id)
    args = capability_spec.probe.args
    specs: list[VersionSpec] = []
    if dependency is not None:
        specs.append(dependency.version)
    probe_spec = VersionSpec(
        minimum_version=args.get("minimum_version"),
        maximum_version=args.get("maximum_version"),
        exact_version=args.get("exact_version"),
        tested_ranges=tuple(args.get("tested_ranges") or ()),
        known_incompatible=tuple(args.get("known_incompatible") or ()),
    )
    if any(
        value
        for value in (
            probe_spec.minimum_version,
            probe_spec.maximum_version,
            probe_spec.exact_version,
            probe_spec.tested_ranges,
            probe_spec.known_incompatible,
        )
    ):
        specs.append(probe_spec)
    return tuple(specs) or (VersionSpec(),)


def version_evidence_line(capability_spec, detected: str | None) -> str | None:
    """Return one command-output line that identifies the canonical capability."""

    if not isinstance(detected, str):
        return None
    args = capability_spec.probe.args
    identity = str(args.get("executable") or args.get("module") or "")
    identity = re.sub(r"[^a-z0-9]+", "", Path(identity).name.lower())
    identity = re.sub(r"\d+$", "", identity)
    for raw_line in detected.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        match = _VERSION_TOKEN_RE.search(line)
        if match is None:
            continue
        if match.start() == 0 and not line[match.end():].strip():
            return line
        normalized_prefix = re.sub(r"[^a-z0-9]+", "", line[:match.start()].lower())
        if identity and normalized_prefix.startswith(identity):
            return line
    return None


_version_evidence_line = version_evidence_line


def classify_capability_version(
    registry: ManifestRegistry,
    capability_spec,
    detected: str | None,
) -> VersionCompatibility:
    """Classify strict version evidence using the canonical dependency spec."""

    evidence = version_evidence_line(capability_spec, detected)
    if evidence is None:
        return VersionCompatibility.UNPARSEABLE
    if ParsedVersion.parse(evidence) is None:
        return VersionCompatibility.UNPARSEABLE
    compatibilities = tuple(
        classify_version(spec, evidence)
        for spec in _capability_version_specs(registry, capability_spec)
    )
    if VersionCompatibility.INCOMPATIBLE in compatibilities:
        return VersionCompatibility.INCOMPATIBLE
    if VersionCompatibility.UNPARSEABLE in compatibilities:
        return VersionCompatibility.UNPARSEABLE
    if VersionCompatibility.SATISFIED_TESTED in compatibilities:
        return VersionCompatibility.SATISFIED_TESTED
    return VersionCompatibility.SATISFIED_UNTESTED


def _expect_id(value: Any, field: str) -> str:
    text = _expect_string(value, field)
    if not _ID_RE.fullmatch(text):
        raise ForensicContractError(f"{field} contains an invalid id: {text!r}")
    return text


def _load_json_file(
    path: Path,
    *,
    label: str,
    max_bytes: int = MAX_FORENSIC_JSON_BYTES,
) -> Mapping[str, Any]:
    path = Path(path)
    try:
        st = path.lstat()
        if stat.S_ISLNK(st.st_mode):
            raise ForensicContractError(f"{label} must not be a symlink: {path}")
        if not stat.S_ISREG(st.st_mode):
            raise ForensicContractError(f"{label} is not a regular file: {path}")
        if type(max_bytes) is not int or max_bytes < 0:
            raise ForensicContractError(f"{label} byte limit must be a non-negative integer")
        if max_bytes > MAX_FORENSIC_JSON_BYTES:
            raise ForensicContractError(
                f"{label} byte limit exceeds hard limit {MAX_FORENSIC_JSON_BYTES}"
            )
        raw = read_regular_file(
            path,
            max_bytes=max_bytes,
            hard_limit=MAX_FORENSIC_JSON_BYTES,
            initial_stat=st,
        )
        text = raw.decode("utf-8")
        payload = json.loads(text)
    except ForensicContractError:
        raise
    except FingerprintLimitExceeded as exc:
        raise ForensicContractError(
            f"{label} exceeds the {max_bytes}-byte observation limit"
        ) from exc
    except OSError as exc:
        raise ForensicContractError(f"cannot read {label}: {exc}") from exc
    except UnicodeDecodeError as exc:
        raise ForensicContractError(f"cannot decode {label}: {exc}") from exc
    except (json.JSONDecodeError, RecursionError) as exc:
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
        health = _expect_string(item.get("health"), f"component {cid} health")
        if health not in _COMPONENT_HEALTH:
            raise ForensicContractError(f"component {cid} has invalid health {health!r}")
        components[cid] = ReceiptComponent(
            component_id=cid,
            display_name=_expect_string(item.get("display_name", cid), f"component {cid} display_name"),
            category=_expect_string(item.get("category", "unknown"), f"component {cid} category"),
            health=health,
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
        mode = _optional_string(item.get("mode"), f"artifact {aid} mode")
        if mode is not None and (len(mode) != 4 or any(char not in "01234567" for char in mode)):
            raise ForensicContractError(f"artifact {aid} mode must be four octal digits or null")
        artifacts[aid] = ReceiptArtifact(
            artifact_id=aid,
            component_id=_expect_id(item.get("component_id"), f"artifact {aid} component_id"),
            path=_expect_string(item.get("path"), f"artifact {aid} path"),
            artifact_type=_expect_string(item.get("type"), f"artifact {aid} type"),
            ownership=_expect_string(item.get("ownership"), f"artifact {aid} ownership"),
            mode=mode,
            sha256=_optional_digest(item.get("sha256"), f"artifact {aid} sha256"),
            immutable_fingerprint=_optional_digest(
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
    if schema not in {LEGACY_HEALTH_SNAPSHOT_SCHEMA, SUPPORTED_HEALTH_SNAPSHOT_SCHEMA}:
        raise ForensicContractError(
            f"unsupported health snapshot schema {schema}; "
            f"artifact path binding requires schema {SUPPORTED_HEALTH_SNAPSHOT_SCHEMA}"
        )

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
        filesystem_type = _optional_string(
            item.get("filesystem_type"), f"health snapshot artifact {aid} filesystem_type"
        )
        if filesystem_type is not None and filesystem_type not in _FILESYSTEM_TYPES:
            raise ForensicContractError(
                f"health snapshot artifact {aid} has invalid filesystem_type {filesystem_type!r}"
            )
        error = _optional_string(item.get("error"), f"health snapshot artifact {aid} error")
        raw_outcome = item.get("outcome")
        if raw_outcome is None:
            outcome = ObservationOutcome.UNKNOWN if error is not None else (
                ObservationOutcome.OBSERVED if exists else ObservationOutcome.MISSING
            )
        else:
            outcome_text = _expect_string(raw_outcome, f"health snapshot artifact {aid} outcome")
            try:
                outcome = ObservationOutcome(outcome_text)
            except ValueError as exc:
                raise ForensicContractError(
                    f"health snapshot artifact {aid} has invalid outcome {outcome_text!r}"
                ) from exc
        if outcome is ObservationOutcome.MISSING and exists:
            raise ForensicContractError(
                f"health snapshot artifact {aid} missing outcome cannot have exists=true"
            )
        if outcome is ObservationOutcome.OBSERVED and not exists and error is None:
            raise ForensicContractError(
                f"health snapshot artifact {aid} observed outcome requires exists=true"
            )
        mode = _optional_string(item.get("mode"), f"health snapshot artifact {aid} mode")
        if mode is not None and (len(mode) != 4 or any(char not in "01234567" for char in mode)):
            raise ForensicContractError(f"health snapshot artifact {aid} mode must be four octal digits or null")
        observed_path = _optional_string(item.get("path"), f"health snapshot artifact {aid} path")
        artifacts[aid] = ArtifactObservation(
            artifact_id=aid,
            exists=exists,
            sha256=_optional_digest(item.get("sha256"), f"health snapshot artifact {aid} sha256"),
            immutable_fingerprint=_optional_digest(
                item.get("immutable_fingerprint"), f"health snapshot artifact {aid} immutable_fingerprint"
            ),
            mode=mode,
            error=error,
            filesystem_type=filesystem_type,
            outcome=outcome,
            path=observed_path,
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


def _blocking_component(registry: ManifestRegistry, component_id: str | None) -> bool:
    component = registry.components.get(component_id or "")
    return component is not None and component.category in {"core", "essential", "fx"}


def capability_requires_success(registry: ManifestRegistry, capability_spec) -> bool:
    return capability_spec.requirement == "required" or (
        capability_spec.requirement == "component"
        and _blocking_component(registry, capability_spec.component_id)
    )


def capability_state_satisfied(
    registry: ManifestRegistry,
    capability_spec,
    state: str,
) -> bool:
    """Return whether a capability state establishes its contract."""

    if state == "pass":
        return True
    return state == "not_applicable" and not capability_requires_success(
        registry, capability_spec
    )


def _capability_failure_severity(registry: ManifestRegistry, capability_spec) -> str:
    """Map a failed capability to the same nuclear boundary as preflight."""

    blocking = _blocking_component(registry, capability_spec.component_id)
    if blocking and capability_spec.requirement == "component":
        return "critical"
    if blocking and capability_spec.requirement == "required" and "runtime" in capability_spec.lifecycle:
        return "critical"
    if capability_spec.requirement == "required":
        return "error"
    return "warning"


def _artifact_failure_severity(registry: ManifestRegistry, artifact_spec) -> str:
    if artifact_spec.required and _blocking_component(registry, artifact_spec.component_id):
        return "critical"
    return "error" if artifact_spec.required else "warning"


def _repair_readiness(
    registry: ManifestRegistry,
    snapshot: CurrentHealthSnapshot,
    drifts: Sequence[DriftRecord] = (),
) -> ReadinessState:
    relevant = [cap for cap in registry.capabilities.values() if _REPAIR_LIFECYCLES.intersection(cap.lifecycle)]
    has_unknown = False
    worst = ReadinessState.HEALTHY
    for cap in relevant:
        requires_success = capability_requires_success(registry, cap)
        current = snapshot.capabilities.get(cap.id)
        if current is None:
            if requires_success:
                has_unknown = True
            continue
        if capability_state_satisfied(registry, cap, current.state):
            compatibility = None
            if capability_version_required(registry, cap):
                compatibility = classify_capability_version(registry, cap, current.version)
            if compatibility is VersionCompatibility.UNPARSEABLE:
                if requires_success:
                    has_unknown = True
                elif worst is not ReadinessState.FAILED:
                    worst = ReadinessState.DEGRADED
            elif compatibility is VersionCompatibility.INCOMPATIBLE:
                if requires_success:
                    worst = ReadinessState.FAILED
                elif worst is not ReadinessState.FAILED:
                    worst = ReadinessState.DEGRADED
            continue
        if current.state in {"unknown", "not_applicable"}:
            if requires_success:
                has_unknown = True
            elif worst is not ReadinessState.FAILED:
                worst = ReadinessState.DEGRADED
        elif requires_success:
            worst = ReadinessState.FAILED
        elif worst is not ReadinessState.FAILED:
            worst = ReadinessState.DEGRADED
    for drift in drifts:
        if drift.error_code not in {
            "RH_FORENSIC_ARTIFACT_PATH_DRIFT",
            "RH_FORENSIC_ARTIFACT_PATH_UNKNOWN",
        }:
            continue
        artifact = registry.artifacts.get(drift.subject_id)
        if artifact is None:
            continue
        if drift.error_code == "RH_FORENSIC_ARTIFACT_PATH_DRIFT" and artifact.required:
            worst = ReadinessState.FAILED
        elif artifact.required:
            has_unknown = True
        elif worst is not ReadinessState.FAILED:
            worst = ReadinessState.DEGRADED
    if worst is ReadinessState.FAILED:
        return ReadinessState.FAILED
    if has_unknown:
        return ReadinessState.UNKNOWN
    return worst


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

    expected_component_ids = set(registry.components)
    required_component_ids = {
        component.id
        for component in registry.components.values()
        if component.category in {"core", "essential", "fx"}
    }
    required_component_ids.update(
        spec.component_id
        for spec in registry.capabilities.values()
        if spec.component_id is not None
        and (
            spec.requirement == "required"
            or (
                spec.requirement == "component"
                and _blocking_component(registry, spec.component_id)
            )
        )
    )
    required_component_ids.update(
        spec.component_id for spec in registry.artifacts.values() if spec.required
    )
    for component_id in sorted(set(receipt.components) - expected_component_ids):
        add(
            DriftKind.MANIFEST,
            "RH_FORENSIC_RECEIPT_UNKNOWN_ID",
            "warning",
            component_id,
            current="unknown",
            summary=f"installed receipt contains non-canonical component record {component_id}",
        )
    for component_id in sorted(required_component_ids - set(receipt.components)):
        add(
            DriftKind.MANIFEST,
            "RH_FORENSIC_COMPONENT_UNKNOWN",
            "warning",
            component_id,
            previous="receipt",
            current="unknown",
            affects_repair=True,
            summary=f"installed receipt omitted required component record {component_id}",
        )
    for capability_id in sorted(set(receipt.capabilities) - set(registry.capabilities)):
        add(
            DriftKind.DEPENDENCY,
            "RH_FORENSIC_RECEIPT_UNKNOWN_ID",
            "warning",
            capability_id,
            current="unknown",
            summary=f"installed receipt contains non-canonical dependency record {capability_id}",
        )
    for artifact_id in sorted(set(receipt.artifacts) - set(registry.artifacts)):
        add(
            DriftKind.ARTIFACT,
            "RH_FORENSIC_RECEIPT_UNKNOWN_ID",
            "warning",
            artifact_id,
            current="unknown",
            summary=f"installed receipt contains non-canonical artifact record {artifact_id}",
        )
    required_capability_ids = {
        spec.id
        for spec in registry.capabilities.values()
        if capability_requires_success(registry, spec)
    }
    required_artifact_ids = {
        spec.id for spec in registry.artifacts.values() if spec.required
    }
    for capability_id in sorted(
        (set(snapshot.capabilities) & set(registry.capabilities))
        - set(receipt.capabilities)
        - required_capability_ids
    ):
        spec = registry.capabilities[capability_id]
        add(
            DriftKind.DEPENDENCY,
            "RH_FORENSIC_RECEIPT_CONTRACT_UNKNOWN",
            "warning",
            capability_id,
            component_id=spec.component_id,
            dependency_id=spec.dependency_id,
            current="unknown",
            affects_runtime="runtime" in spec.lifecycle,
            affects_repair=bool(_REPAIR_LIFECYCLES.intersection(spec.lifecycle)),
            summary=(
                f"current health snapshot contains dependency {capability_id}, "
                "but the installed receipt omitted its matching record"
            ),
        )
    for artifact_id in sorted(
        (set(snapshot.artifacts) & set(registry.artifacts))
        - set(receipt.artifacts)
        - required_artifact_ids
    ):
        spec = registry.artifacts[artifact_id]
        add(
            DriftKind.ARTIFACT,
            "RH_FORENSIC_RECEIPT_CONTRACT_UNKNOWN",
            "warning",
            artifact_id,
            component_id=spec.component_id,
            current="unknown",
            affects_runtime=True,
            affects_repair=True,
            summary=(
                f"current health snapshot contains artifact {artifact_id}, "
                "but the installed receipt omitted its matching record"
            ),
        )
    for component_id, accepted_component in receipt.components.items():
        canonical_component = registry.components.get(component_id)
        if canonical_component is None:
            continue
        if (
            accepted_component.display_name != canonical_component.name
            or accepted_component.category != canonical_component.category
        ):
            add(
                DriftKind.MANIFEST,
                "RH_FORENSIC_RECEIPT_CONTRACT_UNKNOWN",
                "warning",
                component_id,
                component_id=component_id,
                current="unknown",
                affects_repair=True,
                summary=f"installed receipt component {component_id} does not match the canonical component contract",
            )
        for artifact_id in accepted_component.artifact_ids:
            artifact_spec = registry.artifacts.get(artifact_id)
            if artifact_spec is None or artifact_spec.component_id != component_id:
                add(
                    DriftKind.ARTIFACT,
                    "RH_FORENSIC_RECEIPT_CONTRACT_UNKNOWN",
                    "warning",
                    artifact_id,
                    component_id=component_id,
                    current="unknown",
                    affects_repair=True,
                    summary=f"component {component_id} receipt coverage names a non-canonical artifact {artifact_id}",
                )
    for capability_id in sorted(
        cap_id
        for cap_id, spec in registry.capabilities.items()
        if spec.requirement == "required"
        or (
            spec.requirement == "component"
            and _blocking_component(registry, spec.component_id)
        )
    ):
        if capability_id in receipt.capabilities:
            continue
        spec = registry.capabilities[capability_id]
        add(
            DriftKind.DEPENDENCY,
            "RH_FORENSIC_DEPENDENCY_UNKNOWN",
            "warning",
            capability_id,
            component_id=spec.component_id,
            dependency_id=spec.dependency_id,
            previous="receipt",
            current="unknown",
            affects_runtime="runtime" in spec.lifecycle,
            affects_repair=bool(_REPAIR_LIFECYCLES.intersection(spec.lifecycle)),
            summary=f"installed receipt omitted required dependency record {capability_id}",
        )
    for capability_id in sorted(set(snapshot.capabilities) - set(registry.capabilities)):
        add(
            DriftKind.DEPENDENCY,
            "RH_FORENSIC_SNAPSHOT_UNKNOWN_ID",
            "warning",
            capability_id,
            current="unknown",
            summary=f"current health snapshot contains non-canonical dependency record {capability_id}",
        )
    for artifact_id in sorted(set(snapshot.artifacts) - set(registry.artifacts)):
        add(
            DriftKind.ARTIFACT,
            "RH_FORENSIC_SNAPSHOT_UNKNOWN_ID",
            "warning",
            artifact_id,
            current="unknown",
            summary=f"current health snapshot contains non-canonical artifact record {artifact_id}",
        )

    for capid, accepted in receipt.capabilities.items():
        current = snapshot.capabilities.get(capid)
        manifest_cap = registry.capabilities.get(capid)
        if manifest_cap is None:
            continue
        if (
            accepted.component_id != manifest_cap.component_id
            or accepted.requirement != manifest_cap.requirement
            or accepted.lifecycle != manifest_cap.lifecycle
        ):
            add(
                DriftKind.DEPENDENCY,
                "RH_FORENSIC_RECEIPT_CONTRACT_UNKNOWN",
                "warning",
                capid,
                component_id=manifest_cap.component_id,
                dependency_id=manifest_cap.dependency_id,
                current="unknown",
                affects_repair=True,
                summary=f"installed receipt dependency {capid} does not match the canonical capability contract",
            )
        dependency_id = manifest_cap.dependency_id
        runtime = "runtime" in manifest_cap.lifecycle
        repair = bool(_REPAIR_LIFECYCLES.intersection(manifest_cap.lifecycle))
        accepted_compatibility = None
        if accepted.state == "pass" and capability_version_required(registry, manifest_cap):
            accepted_compatibility = classify_capability_version(
                registry, manifest_cap, accepted.version
            )
            if accepted_compatibility is VersionCompatibility.UNPARSEABLE:
                add(
                    DriftKind.DEPENDENCY,
                    "RH_FORENSIC_DEPENDENCY_VERSION_UNKNOWN",
                    "warning",
                    capid,
                    component_id=manifest_cap.component_id,
                    dependency_id=dependency_id,
                    previous=accepted.version,
                    current="unknown",
                    affects_runtime=runtime,
                    affects_repair=repair,
                    summary=f"capability {capid} accepted state lacks parseable required version evidence",
                )
            elif accepted_compatibility is VersionCompatibility.INCOMPATIBLE:
                add(
                    DriftKind.DEPENDENCY,
                    "RH_FORENSIC_DEPENDENCY_VERSION_INCOMPATIBLE",
                    _capability_failure_severity(registry, manifest_cap),
                    capid,
                    component_id=manifest_cap.component_id,
                    dependency_id=dependency_id,
                    previous=accepted.version,
                    current="incompatible",
                    affects_runtime=runtime,
                    affects_repair=repair,
                    summary=f"capability {capid} accepted version violates the canonical compatibility contract",
                )
        if current is None:
            if manifest_cap.requirement != "soft":
                add(
                    DriftKind.DEPENDENCY,
                    "RH_FORENSIC_DEPENDENCY_UNKNOWN",
                    "warning",
                    capid,
                    component_id=manifest_cap.component_id,
                    dependency_id=manifest_cap.dependency_id,
                    previous=accepted.state,
                    current="unknown",
                    affects_runtime="runtime" in manifest_cap.lifecycle,
                    affects_repair=bool(_REPAIR_LIFECYCLES.intersection(manifest_cap.lifecycle)),
                    summary=f"capability {capid} was not included in the current health snapshot",
                )
            continue
        current_state = current.state
        current_compatibility = None
        current_version_bad = False
        current_failure_observed = current.state in {"missing", "failed"}
        if current.state in {"pass", "missing", "failed"} and capability_version_required(registry, manifest_cap):
            current_compatibility = classify_capability_version(
                registry, manifest_cap, current.version
            )
            if current_compatibility is VersionCompatibility.UNPARSEABLE:
                if current.state == "pass":
                    current_state = "unknown"
                add(
                    DriftKind.DEPENDENCY,
                    "RH_FORENSIC_DEPENDENCY_VERSION_UNKNOWN",
                    "warning",
                    capid,
                    component_id=manifest_cap.component_id,
                    dependency_id=dependency_id,
                    previous=accepted.state,
                    current=current.version,
                    affects_runtime=runtime,
                    affects_repair=repair,
                    summary=(
                        f"capability {capid} current state is {current.state} "
                        "but its required version evidence is not parseable"
                    ),
                )
            elif current_compatibility is VersionCompatibility.INCOMPATIBLE:
                if current.state == "pass":
                    current_state = "failed"
                    current_version_bad = True
                add(
                    DriftKind.DEPENDENCY,
                    "RH_FORENSIC_DEPENDENCY_VERSION_INCOMPATIBLE",
                    _capability_failure_severity(registry, manifest_cap),
                    capid,
                    component_id=manifest_cap.component_id,
                    dependency_id=dependency_id,
                    previous=accepted.version,
                    current=current.version,
                    affects_runtime=runtime,
                    affects_repair=repair,
                    summary=f"capability {capid} current version violates the canonical compatibility contract",
                )
        was_ok = (
            capability_state_satisfied(registry, manifest_cap, accepted.state)
            and accepted_compatibility
            not in {VersionCompatibility.UNPARSEABLE, VersionCompatibility.INCOMPATIBLE}
        )
        is_ok = capability_state_satisfied(registry, manifest_cap, current_state)
        if accepted.state == "not_applicable" and capability_requires_success(
            registry, manifest_cap
        ):
            add(
                DriftKind.DEPENDENCY,
                "RH_FORENSIC_DEPENDENCY_UNKNOWN",
                "warning",
                capid,
                component_id=manifest_cap.component_id,
                dependency_id=dependency_id,
                previous="not_applicable",
                current="unknown",
                affects_runtime=runtime,
                affects_repair=repair,
                summary=(
                    f"blocking capability {capid} was marked not_applicable, "
                    "so its health was not established"
                ),
            )
        if current_state == "unknown":
            if current.state == "unknown":
                current_detail = "current state could not be independently established"
            else:
                current_detail = "current version evidence could not be independently established"
            add(
                DriftKind.DEPENDENCY,
                "RH_FORENSIC_DEPENDENCY_UNKNOWN",
                "warning",
                capid,
                component_id=manifest_cap.component_id,
                dependency_id=dependency_id,
                previous=accepted.state,
                current="unknown",
                affects_runtime=runtime,
                affects_repair=repair,
                summary=f"capability {capid} {current_detail}",
            )
        elif current_failure_observed or (was_ok and not is_ok and not current_version_bad):
            severity = _capability_failure_severity(registry, manifest_cap)
            code = "RH_FORENSIC_DEPENDENCY_MISSING" if current_state == "missing" else "RH_FORENSIC_DEPENDENCY_FAILED"
            add(
                DriftKind.DEPENDENCY,
                code,
                severity,
                capid,
                component_id=manifest_cap.component_id,
                dependency_id=dependency_id,
                previous=accepted.state,
                current=current_state,
                affects_runtime=runtime,
                affects_repair=repair,
                summary=(
                    f"capability {capid} remains {current_state} in the current health snapshot"
                    if current_failure_observed and accepted.state == current_state
                    else f"capability {capid} regressed from {accepted.state} to {current_state}"
                ),
            )
        elif accepted.state != current_state and not current_version_bad:
            add(
                DriftKind.DEPENDENCY,
                "RH_FORENSIC_DEPENDENCY_STATE_DRIFT",
                "warning",
                capid,
                component_id=manifest_cap.component_id,
                dependency_id=dependency_id,
                previous=accepted.state,
                current=current_state,
                affects_runtime=runtime,
                affects_repair=repair,
                summary=f"capability {capid} state changed from {accepted.state} to {current_state}",
            )

        if (
            current_state != "unknown"
            and accepted.version
            and current.version
            and accepted.version != current.version
            and accepted_compatibility is not VersionCompatibility.UNPARSEABLE
            and current_compatibility is not VersionCompatibility.UNPARSEABLE
            and current_compatibility is not VersionCompatibility.INCOMPATIBLE
        ):
            add(
                DriftKind.DEPENDENCY,
                "RH_FORENSIC_DEPENDENCY_VERSION_INCOMPATIBLE"
                if current_compatibility is VersionCompatibility.INCOMPATIBLE
                else "RH_FORENSIC_DEPENDENCY_VERSION_DRIFT",
                "error" if current_compatibility is VersionCompatibility.INCOMPATIBLE else "warning",
                capid,
                component_id=manifest_cap.component_id,
                dependency_id=dependency_id,
                previous=accepted.version,
                current=current.version,
                affects_runtime=runtime,
                affects_repair=repair,
                summary=f"capability {capid} version changed from {accepted.version} to {current.version}",
            )

    def check_artifact_path_binding(
        artifact_id: str,
        manifest_artifact,
        accepted: ReceiptArtifact | None,
        current: ArtifactObservation | None,
    ) -> None:
        authorized = resolve_canonical_artifact_path(manifest_artifact.path)
        if authorized is None:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_PATH_UNKNOWN",
                "warning",
                artifact_id,
                component_id=manifest_artifact.component_id,
                previous=manifest_artifact.path,
                current="unresolved",
                affects_runtime=True,
                affects_repair=True,
                summary=(
                    f"artifact {artifact_id} canonical manifest path cannot be resolved "
                    "using the allowed path-token environment"
                ),
            )
            return

        unknown_reasons: list[str] = []
        if accepted is None:
            unknown_reasons.append("installed receipt artifact record is missing")
        else:
            observed = normalize_observed_artifact_path(accepted.path)
            if observed is None:
                unknown_reasons.append("installed receipt path is missing, relative, or ambiguous")
            elif observed != authorized:
                add(
                    DriftKind.ARTIFACT,
                    "RH_FORENSIC_ARTIFACT_PATH_DRIFT",
                    _artifact_failure_severity(registry, manifest_artifact),
                    artifact_id,
                    component_id=manifest_artifact.component_id,
                    previous=authorized,
                    current=observed,
                    affects_runtime=True,
                    affects_repair=True,
                    summary=(
                        f"artifact {artifact_id} receipt path {observed} does not match "
                        f"the canonical authorized path {authorized}"
                    ),
                )

        if current is None:
            unknown_reasons.append("current health snapshot artifact record is missing")
        elif snapshot.schema_version < SUPPORTED_HEALTH_SNAPSHOT_SCHEMA:
            unknown_reasons.append(
                f"current path evidence uses health snapshot schema {snapshot.schema_version}, "
                "which predates artifact path binding"
            )
        else:
            observed = normalize_observed_artifact_path(current.path)
            if observed is None:
                unknown_reasons.append("current path is missing, relative, or ambiguous")
            elif observed != authorized:
                add(
                    DriftKind.ARTIFACT,
                    "RH_FORENSIC_ARTIFACT_PATH_DRIFT",
                    _artifact_failure_severity(registry, manifest_artifact),
                    artifact_id,
                    component_id=manifest_artifact.component_id,
                    previous=authorized,
                    current=observed,
                    affects_runtime=True,
                    affects_repair=True,
                    summary=(
                        f"artifact {artifact_id} current path {observed} does not match "
                        f"the canonical authorized path {authorized}"
                    ),
                )

        if unknown_reasons:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_PATH_UNKNOWN",
                "warning",
                artifact_id,
                component_id=manifest_artifact.component_id,
                previous=authorized,
                current="unknown",
                affects_runtime=True,
                affects_repair=True,
                summary=(
                    f"artifact {artifact_id} path evidence is incomplete: "
                    f"{'; '.join(unknown_reasons)}"
                ),
            )

    for artifact_id in sorted(registry.artifacts):
        manifest_artifact = registry.artifacts[artifact_id]
        check_artifact_path_binding(
            artifact_id,
            manifest_artifact,
            receipt.artifacts.get(artifact_id),
            snapshot.artifacts.get(artifact_id),
        )

    for aid, accepted in receipt.artifacts.items():
        current = snapshot.artifacts.get(aid)
        manifest_artifact = registry.artifacts.get(aid)
        if manifest_artifact is None:
            continue
        invalid_accepted_fields = [
            field
            for field, value in (
                ("mode", accepted.mode),
                ("sha256", accepted.sha256),
                ("immutable_fingerprint", accepted.immutable_fingerprint),
            )
            if (
                (field == "mode" and not _valid_mode(value))
                or (field in {"sha256", "immutable_fingerprint"} and not _valid_digest(value))
            )
        ]
        if invalid_accepted_fields:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_UNKNOWN",
                "warning",
                aid,
                component_id=manifest_artifact.component_id,
                previous="receipt",
                current="unknown",
                affects_runtime=True,
                summary=(
                    f"artifact {aid} receipt evidence is invalid: "
                    f"{', '.join(invalid_accepted_fields)}"
                ),
            )
            continue
        if (
            accepted.component_id != manifest_artifact.component_id
            or accepted.artifact_type != manifest_artifact.type
            or accepted.ownership != manifest_artifact.ownership
        ):
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_RECEIPT_CONTRACT_UNKNOWN",
                "warning",
                aid,
                component_id=manifest_artifact.component_id,
                current="unknown",
                affects_repair=True,
                summary=f"installed receipt artifact {aid} does not match the canonical artifact contract",
            )
        accepted_component = receipt.components.get(manifest_artifact.component_id)
        if accepted_component is None or aid not in accepted_component.artifact_ids:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_RECEIPT_CONTRACT_UNKNOWN",
                "warning",
                aid,
                component_id=manifest_artifact.component_id,
                current="unknown",
                affects_repair=True,
                summary=f"installed receipt artifact {aid} is omitted from its component coverage",
            )
        if manifest_artifact.mode is not None and accepted.mode is not None and accepted.mode != manifest_artifact.mode:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_RECEIPT_CONTRACT_UNKNOWN",
                "warning",
                aid,
                component_id=manifest_artifact.component_id,
                previous=manifest_artifact.mode,
                current=accepted.mode,
                affects_repair=True,
                summary=f"installed receipt artifact {aid} mode does not match the canonical artifact contract",
            )
        required_fields = artifact_integrity_fields(manifest_artifact)
        missing_accepted = sorted(
            field for field in required_fields if getattr(accepted, field) is None
        )
        if missing_accepted:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_UNKNOWN",
                "warning",
                aid,
                component_id=manifest_artifact.component_id,
                previous="present",
                current="unknown",
                affects_runtime=True,
                summary=f"artifact {aid} receipt evidence is incomplete: missing {', '.join(missing_accepted)}",
            )
        if current is None:
            continue
        invalid_current_fields = [
            field
            for field, value in (
                ("exists", current.exists),
                ("sha256", current.sha256),
                ("immutable_fingerprint", current.immutable_fingerprint),
                ("mode", current.mode),
                ("filesystem_type", current.filesystem_type),
                ("error", current.error),
            )
            if (
                (field == "exists" and type(value) is not bool)
                or (field in {"sha256", "immutable_fingerprint"} and not _valid_digest(value))
                or (field == "mode" and not _valid_mode(value))
                or (field == "filesystem_type" and value is not None and value not in _FILESYSTEM_TYPES)
                or (field == "error" and value is not None and not isinstance(value, str))
            )
        ]
        if invalid_current_fields:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_UNKNOWN",
                "warning",
                aid,
                component_id=manifest_artifact.component_id,
                previous="present",
                current="unknown",
                affects_runtime=True,
                summary=(
                    f"artifact {aid} observation evidence is invalid: "
                    f"{', '.join(invalid_current_fields)}"
                ),
            )
            continue
        if current.outcome is None:
            outcome = (
                ObservationOutcome.UNKNOWN
                if current.error is not None
                else ObservationOutcome.OBSERVED
                if current.exists
                else ObservationOutcome.MISSING
            )
        else:
            outcome = _observation_outcome(current.outcome)
        if outcome is None or (
            outcome is ObservationOutcome.MISSING and current.exists
        ) or (
            outcome is ObservationOutcome.OBSERVED
            and not current.exists
            and current.error is None
        ):
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_UNKNOWN",
                "warning",
                aid,
                component_id=manifest_artifact.component_id,
                previous="present",
                current="unknown",
                affects_runtime=True,
                summary=f"artifact {aid} observation outcome is invalid or contradictory",
            )
            continue
        if outcome in {ObservationOutcome.UNKNOWN, ObservationOutcome.LIMIT_EXCEEDED}:
            detail = current.error or (
                "observation exceeded its resource limit"
                if outcome is ObservationOutcome.LIMIT_EXCEEDED
                else "observation could not be independently established"
            )
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_UNKNOWN",
                "warning",
                aid,
                component_id=manifest_artifact.component_id,
                previous="present",
                current="unknown",
                affects_runtime=True,
                summary=f"artifact {aid} observation was inconclusive: {detail}",
            )
            continue
        if current.error is not None:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_UNKNOWN",
                "warning",
                aid,
                component_id=manifest_artifact.component_id,
                previous="present",
                current="unknown",
                affects_runtime=True,
                summary=f"artifact {aid} observation was inconclusive: {current.error}",
            )
            continue
        if not current.exists:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_MISSING",
                _artifact_failure_severity(registry, manifest_artifact),
                aid,
                component_id=manifest_artifact.component_id,
                previous="present",
                current="missing",
                affects_runtime=True,
                summary=f"artifact {aid} recorded by the installed receipt is now missing",
            )
            continue
        expected_type = canonical_filesystem_type(manifest_artifact.type)
        if current.filesystem_type is None:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_UNKNOWN",
                "warning",
                aid,
                component_id=manifest_artifact.component_id,
                previous="present",
                current="unknown",
                affects_runtime=True,
                summary=f"artifact {aid} observation is incomplete: missing filesystem_type",
            )
        elif current.filesystem_type != expected_type:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_TYPE_DRIFT",
                _artifact_failure_severity(registry, manifest_artifact),
                aid,
                component_id=manifest_artifact.component_id,
                previous=expected_type,
                current=current.filesystem_type,
                affects_runtime=True,
                summary=f"artifact {aid} has filesystem type {current.filesystem_type}, expected {expected_type}",
            )
            continue
        if manifest_artifact.type == "executable" and current.mode is not None:
            try:
                executable_mode = bool(int(current.mode, 8) & 0o111)
            except (TypeError, ValueError):
                executable_mode = False
            if not executable_mode:
                add(
                    DriftKind.ARTIFACT,
                    "RH_FORENSIC_ARTIFACT_MODE_DRIFT",
                    _artifact_failure_severity(registry, manifest_artifact),
                    aid,
                    component_id=manifest_artifact.component_id,
                    previous="executable-bit",
                    current=current.mode,
                    affects_runtime=True,
                    summary=f"executable artifact {aid} has no executable permission bit",
                )
        missing_current = sorted(
            field for field in required_fields if getattr(current, field) is None
        )
        if missing_current:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_UNKNOWN",
                "warning",
                aid,
                component_id=manifest_artifact.component_id,
                previous="present",
                current="unknown",
                affects_runtime=True,
                summary=f"artifact {aid} observation is incomplete: missing {', '.join(missing_current)}",
            )
        if (
            "sha256" in required_fields
            and accepted.sha256
            and current.sha256
            and accepted.sha256 != current.sha256
        ):
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
            "immutable_fingerprint" in required_fields
            and accepted.immutable_fingerprint
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
        if manifest_artifact.mode and current.mode and manifest_artifact.mode != current.mode:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_MODE_DRIFT",
                _artifact_failure_severity(registry, manifest_artifact),
                aid,
                component_id=manifest_artifact.component_id,
                previous=manifest_artifact.mode,
                current=current.mode,
                affects_runtime=True,
                summary=f"artifact {aid} mode differs from the canonical mode {manifest_artifact.mode}",
            )
        if "mode" in required_fields and accepted.mode and current.mode and accepted.mode != current.mode:
            add(
                DriftKind.ARTIFACT,
                "RH_FORENSIC_ARTIFACT_MODE_DRIFT",
                _artifact_failure_severity(registry, manifest_artifact),
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
    if snapshot.activation_state == "unknown":
        add(
            DriftKind.RUNTIME,
            "RH_FORENSIC_ACTIVATION_UNKNOWN",
            "warning",
            "runtime.activation",
            previous=receipt.activation_state,
            current="unknown",
            affects_runtime=True,
            summary="current activation state could not be independently established",
        )
    elif snapshot.activation_state == "failed":
        add(
            DriftKind.RUNTIME,
            "RH_FORENSIC_ACTIVATION_FAILED",
            "critical",
            "runtime.activation",
            previous=receipt.activation_state,
            current="failed",
            affects_runtime=True,
            summary="current activation probe reports a failed runtime activation",
        )
    if snapshot.runtime_health == "unknown":
        add(
            DriftKind.RUNTIME,
            "RH_FORENSIC_RUNTIME_HEALTH_UNKNOWN",
            "warning",
            "runtime.health",
            previous=receipt.runtime_health,
            current="unknown",
            affects_runtime=True,
            summary="current runtime health could not be independently established",
        )
    elif snapshot.runtime_health == "failed":
        add(
            DriftKind.RUNTIME,
            "RH_FORENSIC_RUNTIME_HEALTH_FAILED",
            "critical",
            "runtime.health",
            previous=receipt.runtime_health,
            current="failed",
            affects_runtime=True,
            summary="current runtime health is failed",
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
        repair_readiness=_repair_readiness(registry, snapshot, drifts),
        selected_health_check_ids=select_health_checks(
            registry, context=health_context, max_cost=max_health_cost
        ),
        drifts=tuple(drifts),
        incidents=tuple(incidents),
    )
