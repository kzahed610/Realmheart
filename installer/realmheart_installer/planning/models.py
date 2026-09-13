"""Serializable Phase-9 installation planning models.

These objects describe *intent*.  They contain no installer callables and no
mutation methods, so the exact same plan can be rendered by dry-run and consumed
later by the live executor.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from ..environment.preflight import EnvironmentSnapshot
from ..models import FxCompatibility, InstallMode, Reversibility
from ..package_manager.base import DependencyPackagePlan


class PlanState(str, Enum):
    READY = "ready"
    BLOCKED = "blocked"


class PackageActionKind(str, Enum):
    INSTALL = "install"
    UPGRADE = "upgrade"


class BackupKind(str, Enum):
    PERMANENT_BASELINE = "permanent_baseline"
    PRE_ADOPTION = "pre_adoption"
    PREVIOUS_VERSION = "previous_version"
    PRESERVE_EXISTING_BASELINE = "preserve_existing_baseline"


class ConfigActionKind(str, Enum):
    FULL_TREE_REPLACE = "full_tree_replace"
    MANAGED_BLOCK = "managed_block"
    OWNED_FILE = "owned_file"
    RENDERED_FILE = "rendered_file"
    SHARED_SEED = "shared_seed"
    GENERATED_STATE = "generated_state"
    READ_ONLY = "read_only"


class ArtifactCommitClass(str, Enum):
    STAGED_PAYLOAD = "staged_payload"
    USER_COMMIT = "user_commit"
    PRIVILEGED_COMMIT = "privileged_commit"
    GENERATED = "generated"


class ServiceActionKind(str, Enum):
    DAEMON_RELOAD = "daemon_reload"
    ENABLE_START = "enable_start"
    ENABLE_ONLY = "enable_only"
    INSTALL_ONLY = "install_only"


@dataclass(frozen=True)
class InstallLayout:
    prefix: str
    libexec: str
    sysconf: str
    home: str
    xdg_config_home: str
    xdg_state_home: str


@dataclass(frozen=True)
class PackageAction:
    package: str
    action: PackageActionKind
    installed_version: str | None
    repository_version: str | None
    required_by: tuple[str, ...]


@dataclass(frozen=True)
class BackupTarget:
    label: str
    path: str
    fingerprint: str
    exists: bool
    estimated_bytes: int | None
    privileged: bool = False


@dataclass(frozen=True)
class BackupAction:
    id: str
    kind: BackupKind
    destination: str
    required: bool
    already_exists: bool
    reason: str
    targets: tuple[BackupTarget, ...]


@dataclass(frozen=True)
class ConfigAction:
    id: str
    component_id: str
    kind: ConfigActionKind
    target: str
    source: str | None
    will_mutate: bool
    reason: str
    backup_policy: str
    reversibility: Reversibility
    precondition_fingerprint: str
    preserve: tuple[str, ...] = ()
    render_strategy: str | None = None
    render_values: tuple[tuple[str, str], ...] = ()
    mode: str | None = None


@dataclass(frozen=True)
class ArtifactAction:
    artifact_id: str
    component_id: str
    target: str
    artifact_type: str
    ownership: str
    commit_class: ArtifactCommitClass
    required: bool
    source: str | None
    privileged: bool


@dataclass(frozen=True)
class PlannedComponent:
    order: int
    id: str
    name: str
    category: str
    stage: str
    component_version: str
    realmheart_dependencies: tuple[str, ...]
    capability_ids: tuple[str, ...]
    build_units: tuple[str, ...]
    artifact_ids: tuple[str, ...]
    health_check_ids: tuple[str, ...]
    requires_installer_binding: bool
    dependency_state: str
    dependency_reasons: tuple[str, ...]


@dataclass(frozen=True)
class PlannedBuildUnit:
    id: str
    cmake_target: str | None
    component_ids: tuple[str, ...]
    artifact_ids: tuple[str, ...]
    build_dependencies: tuple[str, ...]
    abi_sensitive_dependencies: tuple[str, ...]
    rebuild_on_dependency_change: tuple[str, ...]


@dataclass(frozen=True)
class FxPlan:
    required: bool
    compatibility: FxCompatibility
    action: str
    reason: str
    hyprland_version: str | None
    hyprland_commit: str | None
    hyprland_abi_hash: str | None
    build_unit: str
    build_id: str
    plugin_artifact_id: str
    loader_artifact_id: str
    rebuild_on_dependency_change: tuple[str, ...]


@dataclass(frozen=True)
class PrivilegedAction:
    id: str
    component_id: str
    target: str
    action: str
    owner: str
    group: str
    mode: str
    reversibility: Reversibility
    precondition_fingerprint: str


@dataclass(frozen=True)
class ServiceAction:
    id: str
    service: str
    component_id: str | None
    action: ServiceActionKind
    reason: str


@dataclass(frozen=True)
class PlannedHealthCheck:
    id: str
    component_id: str
    check: str
    artifact_id: str | None
    cost: str
    side_effects: str
    timeout_ms: int
    contexts: tuple[str, ...]




@dataclass(frozen=True)
class BuildVerification:
    id: str
    argv: tuple[str, ...]
    timeout_seconds: int
    environment: tuple[tuple[str, str], ...] = ()


@dataclass(frozen=True)
class BuildPlan:
    build_dir: str
    stage_dir: str
    generator: str
    build_type: str
    install_prefix: str
    cmake_executable: str
    ninja_executable: str
    configure_args: tuple[str, ...]
    build_environment: tuple[tuple[str, str], ...]
    install_environment: tuple[tuple[str, str], ...]
    source_prerequisites: tuple[str, ...]
    verification: tuple[BuildVerification, ...]
    allowed_uncommitted_stage_paths: tuple[str, ...]
    side_effects_disabled: bool


@dataclass(frozen=True)
class ActivationPlan:
    expected_state: str
    reason: str
    requires_fresh_session_if_unproven: bool

@dataclass(frozen=True)
class DiskEstimate:
    backup_bytes: int
    config_staging_bytes: int
    known_minimum_bytes: int
    free_bytes_at_config_root: int | None
    known_minimum_fits: bool | None
    complete: bool
    note: str


@dataclass(frozen=True)
class InstallationPlan:
    schema_version: int
    transaction_id: str
    mode: InstallMode
    current_version: str | None
    target_version: str
    source_revision: str | None
    source_dirty: bool | None
    environment: EnvironmentSnapshot
    manifest_digest: str
    manifest_schema_version: int
    package_plan: DependencyPackagePlan
    package_actions: tuple[PackageAction, ...]
    backup_actions: tuple[BackupAction, ...]
    config_actions: tuple[ConfigAction, ...]
    artifact_actions: tuple[ArtifactAction, ...]
    components: tuple[PlannedComponent, ...]
    build_units: tuple[PlannedBuildUnit, ...]
    fx_plan: FxPlan
    privileged_actions: tuple[PrivilegedAction, ...]
    service_actions: tuple[ServiceAction, ...]
    health_checks: tuple[PlannedHealthCheck, ...]
    build: BuildPlan
    activation: ActivationPlan
    disk: DiskEstimate
    layout: InstallLayout
    warnings: tuple[str, ...]
    blockers: tuple[str, ...]
    state: PlanState
    plan_digest: str

    @property
    def ready(self) -> bool:
        return self.state is PlanState.READY
