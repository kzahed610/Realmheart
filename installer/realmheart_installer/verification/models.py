"""Serializable Phase-13 installation verification models.

The verification engine records observed machine state after deployment.  It is
intentionally separate from desired-state planning: receipt inputs are derived
from these observations rather than copied from the manifest.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class VerificationCheckState(str, Enum):
    PASS = "pass"
    WARNING = "warning"
    FAILED = "failed"
    BLOCKED = "blocked"
    NOT_APPLICABLE = "not_applicable"
    PENDING = "pending"


class VerificationClass(str, Enum):
    STRUCTURAL = "structural"
    DEPENDENCY = "dependency"
    CONFIGURATION = "configuration"
    COMPONENT = "component"
    SMOKE = "smoke"
    SECURITY = "security"
    SERVICE = "service"
    FX = "fx"
    ACTIVATION = "activation"
    PROVENANCE = "provenance"


class ComponentHealthState(str, Enum):
    HEALTHY = "healthy"
    DEGRADED = "degraded"
    FAILED = "failed"
    BLOCKED = "blocked"
    NOT_APPLICABLE = "not_applicable"
    PENDING_ACTIVATION = "pending_activation"


class InstallHealthState(str, Enum):
    HEALTHY = "healthy"
    DEGRADED = "degraded"
    FAILED = "failed"


class ActivationState(str, Enum):
    ACTIVE = "active"
    PENDING_SESSION_RESTART = "pending_session_restart"
    UNKNOWN = "unknown"


class RuntimeHealthState(str, Enum):
    HEALTHY = "healthy"
    DEGRADED = "degraded"
    FAILED = "failed"
    UNKNOWN = "unknown"


@dataclass(frozen=True)
class VerificationCheckResult:
    id: str
    component_id: str
    check_class: VerificationClass
    state: VerificationCheckState
    summary: str
    artifact_id: str | None = None
    capability_id: str | None = None
    observed: str | None = None
    expected: str | None = None
    critical: bool = False

    @property
    def ok(self) -> bool:
        return self.state in {VerificationCheckState.PASS, VerificationCheckState.NOT_APPLICABLE}


@dataclass(frozen=True)
class ObservedArtifactIdentity:
    artifact_id: str
    component_id: str
    path: str
    artifact_type: str
    ownership: str
    required: bool
    exists: bool
    filesystem_type: str | None
    mode: str | None
    uid: int | None
    gid: int | None
    size_bytes: int | None
    sha256: str | None
    immutable_fingerprint: str | None


@dataclass(frozen=True)
class ObservedDependency:
    capability_id: str
    component_id: str | None
    requirement: str
    lifecycle: tuple[str, ...]
    state: str
    detail: str
    version: str | None
    executable: str | None
    observed_during_verification: bool


@dataclass(frozen=True)
class ComponentVerification:
    component_id: str
    display_name: str
    category: str
    state: ComponentHealthState
    checks: tuple[VerificationCheckResult, ...]
    blocked_by: tuple[str, ...]
    warnings: tuple[str, ...]
    artifact_ids: tuple[str, ...]
    build_unit_ids: tuple[str, ...]


@dataclass(frozen=True)
class ActivationVerification:
    state: ActivationState
    runtime_health: RuntimeHealthState
    reason: str
    core_service_enabled: bool | None
    core_service_active: bool | None
    fx_runtime: "FxRuntimeIdentity" | None = None


@dataclass(frozen=True)
class FxRuntimeIdentity:
    plugin_listed: bool | None
    identity_available: bool | None
    build_id: str | None
    realmheart_version: str | None
    hyprland_commit: str | None
    hyprland_abi_hash: str | None
    hyprland_dirty: bool | None
    matches_validated_build: bool | None
    detail: str


@dataclass(frozen=True)
class FxRebuildTriggerInput:
    capability_id: str
    build_unit_id: str
    trigger: str
    observed_version: str | None
    observed_commit: str | None
    observed_abi_hash: str | None


@dataclass(frozen=True)
class FxReceiptInput:
    required: bool
    compatibility: str
    build_unit_id: str
    build_id: str
    plugin_artifact_id: str
    loader_artifact_id: str
    plugin_sha256: str | None
    loader_path: str | None
    hyprland_version: str | None
    hyprland_commit: str | None
    hyprland_abi_hash: str | None
    rebuild_triggers: tuple[FxRebuildTriggerInput, ...]


@dataclass(frozen=True)
class ReceiptBuildUnitInput:
    build_unit_id: str
    component_ids: tuple[str, ...]
    health: str
    artifact_ids: tuple[str, ...]
    artifact_sha256: tuple[tuple[str, str], ...]
    abi_sensitive_dependencies: tuple[str, ...]


@dataclass(frozen=True)
class ReceiptInputAssembly:
    schema_version: int
    realmheart_version: str
    manifest_schema_version: int
    manifest_set_sha256: str
    installer_version: str
    transaction_id: str
    install_health: str
    activation_state: str
    runtime_health: str
    verified_at: str
    components: tuple[ComponentVerification, ...]
    dependencies: tuple[ObservedDependency, ...]
    artifacts: tuple[ObservedArtifactIdentity, ...]
    build_units: tuple[ReceiptBuildUnitInput, ...]
    fx: FxReceiptInput
    build_provenance: dict[str, object] | None


@dataclass(frozen=True)
class VerificationReport:
    schema_version: int
    transaction_id: str
    realmheart_version: str
    manifest_digest: str
    plan_digest: str
    install_health: InstallHealthState
    activation: ActivationVerification
    components: tuple[ComponentVerification, ...]
    checks: tuple[VerificationCheckResult, ...]
    artifacts: tuple[ObservedArtifactIdentity, ...]
    dependencies: tuple[ObservedDependency, ...]
    receipt_inputs: ReceiptInputAssembly
    warnings: tuple[str, ...]
    blockers: tuple[str, ...]

    @property
    def ok(self) -> bool:
        return self.install_health is not InstallHealthState.FAILED
