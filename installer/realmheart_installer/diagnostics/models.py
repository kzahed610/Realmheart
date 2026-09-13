"""Serializable Phase-15 diagnostic/incident-report models."""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class DiagnosticSeverity(str, Enum):
    INFO = "info"
    WARNING = "warning"
    ERROR = "error"
    CRITICAL = "critical"


@dataclass(frozen=True)
class DiagnosticDisplay:
    name: str
    width: int | None
    height: int | None
    refresh_hz: float | None
    scale: float | None
    x: int | None
    y: int | None
    focused: bool


@dataclass(frozen=True)
class DiagnosticEnvironment:
    distribution_id: str
    distribution: str
    distribution_version: str | None
    architecture: str
    kernel: str
    package_manager: str | None
    session_type: str | None
    wayland: bool
    systemd_user_available: bool
    hyprland_version: str | None
    hyprland_compatibility: str
    hyprland_commit: str | None
    hyprland_abi_hash: str | None
    hyprland_dirty: bool | None
    displays: tuple[DiagnosticDisplay, ...]


@dataclass(frozen=True)
class DiagnosticEvent:
    sequence: int
    stage: str
    severity: DiagnosticSeverity
    code: str
    summary: str
    component_id: str | None = None
    check_id: str | None = None


@dataclass(frozen=True)
class RootFailure:
    root_id: str
    component_id: str | None
    component_name: str | None
    severity: DiagnosticSeverity
    error_codes: tuple[str, ...]
    failed_checks: tuple[str, ...]
    summary: str
    affected_components: tuple[str, ...]


@dataclass(frozen=True)
class BlockedImpact:
    component_id: str
    component_name: str
    blocked_by: tuple[str, ...]


@dataclass(frozen=True)
class RollbackAvailability:
    permanent_baseline_available: bool
    version_snapshot_count: int
    recovery_candidate_count: int
    transaction_journal_available: bool
    automatic_recovery_possible: bool | None
    note: str


@dataclass(frozen=True)
class VerificationDiagnosticSummary:
    install_health: str | None
    activation_state: str | None
    runtime_health: str | None
    component_count: int
    check_count: int
    immutable_artifact_count: int


@dataclass(frozen=True)
class BuildDiagnosticSummary:
    state: str
    configured: bool
    required_targets_built: bool
    self_checks_passed: bool
    staged_install_completed: bool
    live_targets_unchanged: bool
    staged_payload_bytes: int
    realmheart_version: str | None
    source_revision: str | None
    source_dirty: bool | None
    hyprland_version: str | None
    hyprland_commit: str | None
    hyprland_abi_hash: str | None
    fx_build_id: str | None


@dataclass(frozen=True)
class DiagnosticReport:
    schema_version: int
    incident_id: str
    incident_fingerprint: str
    created_at: str
    installer_version: str
    transaction_id: str
    mode: str | None
    installation_origin: str | None
    current_version: str | None
    target_version: str | None
    manifest_digest: str | None
    plan_digest: str | None
    environment: DiagnosticEnvironment
    verification: VerificationDiagnosticSummary
    build: BuildDiagnosticSummary | None
    events: tuple[DiagnosticEvent, ...]
    root_failures: tuple[RootFailure, ...]
    blocked_components: tuple[BlockedImpact, ...]
    warnings: tuple[str, ...]
    rollback: RollbackAvailability
    privacy_contract: tuple[str, ...]

    @property
    def has_failures(self) -> bool:
        return bool(self.root_failures)
