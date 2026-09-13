"""Serializable Phase-10 native build and staged-install results."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class BuildStageState(str, Enum):
    PASS = "pass"
    FAILED = "failed"


@dataclass(frozen=True)
class BuildCommandRecord:
    label: str
    argv: tuple[str, ...]
    returncode: int
    ok: bool
    timed_out: bool
    stdout_tail: str
    stderr_tail: str


@dataclass(frozen=True)
class BuildUnitResult:
    build_unit_id: str
    cmake_target: str
    ok: bool
    artifact_ids: tuple[str, ...]
    reason: str | None = None


@dataclass(frozen=True)
class StagedArtifactResult:
    artifact_id: str
    target_path: str
    staged_path: str
    artifact_type: str
    required: bool
    exists: bool
    type_ok: bool
    executable_ok: bool | None
    mode: str | None
    size_bytes: int | None
    sha256: str | None
    fingerprint: str | None
    reason: str | None = None

    @property
    def ok(self) -> bool:
        return self.exists and self.type_ok and self.reason is None and self.executable_ok is not False


@dataclass(frozen=True)
class BuildProvenance:
    realmheart_version: str
    source_revision: str | None
    source_dirty: bool | None
    manifest_digest: str
    plan_digest: str
    cmake_version: str | None
    ninja_version: str | None
    cxx_compiler: str | None
    cxx_compiler_version: str | None
    cmake_generator: str | None
    cmake_build_type: str | None
    cmake_install_prefix: str | None
    cmake_install_sysconfdir: str | None
    eventd_autostart: str | None
    hyprland_version: str | None
    hyprland_commit: str | None
    hyprland_abi_hash: str | None
    fx_build_id: str | None


@dataclass(frozen=True)
class BuildStageReport:
    schema_version: int
    transaction_id: str
    state: BuildStageState
    build_dir: str
    stage_dir: str
    configured: bool
    required_targets_built: bool
    self_checks_passed: bool
    staged_install_completed: bool
    live_targets_unchanged: bool
    drifted_live_targets: tuple[str, ...]
    eventd_unit_unchanged: bool
    eventd_runtime_signature_unchanged: bool | None
    build_units: tuple[BuildUnitResult, ...]
    artifacts: tuple[StagedArtifactResult, ...]
    commands: tuple[BuildCommandRecord, ...]
    provenance: BuildProvenance | None
    staged_payload_bytes: int
    accounted_uncommitted_stage_paths: tuple[str, ...]
    unexpected_stage_paths: tuple[str, ...]
    warnings: tuple[str, ...]
    blockers: tuple[str, ...]

    @property
    def ok(self) -> bool:
        return self.state is BuildStageState.PASS
