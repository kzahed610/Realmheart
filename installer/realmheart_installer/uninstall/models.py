"""Serializable Phase-17 uninstall planning/execution models."""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class UninstallConfigAction(str, Enum):
    KEEP_CURRENT = "keep-current"
    RESTORE_BASELINE = "restore-baseline"


class FootprintAction(str, Enum):
    REMOVE = "remove"
    RESTORE_PREIMAGE = "restore_preimage"
    PRESERVE = "preserve"
    PRESERVE_CONFLICT = "preserve_conflict"


class DifferenceKind(str, Enum):
    ADDED = "added"
    REMOVED = "removed"
    CHANGED = "changed"
    TYPE_CHANGED = "type_changed"


@dataclass(frozen=True)
class DifferenceEntry:
    target: str
    relative_path: str
    kind: DifferenceKind


@dataclass(frozen=True)
class ConfigComparison:
    target: str
    baseline_label: str
    changed: bool
    current_fingerprint: str
    baseline_fingerprint: str | None
    differences: tuple[DifferenceEntry, ...]


@dataclass(frozen=True)
class BaselineEntry:
    label: str
    target: str
    existed: bool
    backup_path: str | None
    baseline_fingerprint: str | None
    source_type: str | None = None
    source_mode: str | None = None
    source_uid: int | None = None
    source_gid: int | None = None


@dataclass(frozen=True)
class FootprintEntry:
    artifact_id: str
    target: str
    artifact_type: str
    ownership: str
    current_exists: bool
    baseline_existed: bool | None
    baseline_backup_path: str | None
    baseline_source_type: str | None
    baseline_source_mode: str | None
    baseline_source_uid: int | None
    baseline_source_gid: int | None
    last_managed_fingerprint: str | None
    current_fingerprint: str
    diverged: bool
    keep_current_action: FootprintAction
    reason: str
    privileged: bool = False


@dataclass(frozen=True)
class ServiceState:
    service: str
    enabled_before_install: bool | None
    active_before_install: bool | None


@dataclass(frozen=True)
class PackageCleanupCandidate:
    package: str
    version_after: str | None
    required_by: tuple[str, ...]


@dataclass(frozen=True)
class UninstallPlan:
    schema_version: int
    transaction_id: str
    installed_version: str | None
    install_transaction_id: str | None
    receipt_path: str
    managed_install: bool
    baseline_path: str
    baseline_available: bool
    baseline_valid: bool
    baseline_entries: tuple[BaselineEntry, ...]
    footprint: tuple[FootprintEntry, ...]
    comparisons: tuple[ConfigComparison, ...]
    service_states: tuple[ServiceState, ...]
    package_cleanup_candidates: tuple[PackageCleanupCandidate, ...]
    shared_seed_entries: tuple[BaselineEntry, ...]
    preserved_paths: tuple[str, ...]
    warnings: tuple[str, ...]
    blockers: tuple[str, ...]

    @property
    def ready(self) -> bool:
        return self.managed_install and not self.blockers

    @property
    def has_config_divergence(self) -> bool:
        return any(item.changed for item in self.comparisons)


@dataclass(frozen=True)
class PackageCleanupResult:
    requested: tuple[str, ...]
    removed: tuple[str, ...]
    error: str | None = None


@dataclass(frozen=True)
class UninstallResult:
    transaction_id: str
    config_action: UninstallConfigAction
    completed: bool
    rolled_back: bool
    safety_snapshot: str | None
    receipt_retired_to: str | None
    removed_paths: tuple[str, ...]
    restored_paths: tuple[str, ...]
    preserved_paths: tuple[str, ...]
    warnings: tuple[str, ...]
    errors: tuple[str, ...]
    package_cleanup: PackageCleanupResult | None
    exit_code: int
