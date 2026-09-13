"""Phase-5 environment snapshot and support decision."""

from __future__ import annotations

import os
import platform
import shutil
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Mapping

from realmheart_maintenance.manifest import ManifestError, ManifestRegistry, load_manifest

from ..context import XdgPaths
from .capabilities import CapabilityResult, CapabilityScanner, RequirementLevel, required_failures
from .command import CommandRunner
from .detect import DistroInfo, detect_displays, detect_distro, detect_hyprland, detect_package_manager, detect_session
from .installation import InstallationState, detect_installation_state
from .support import HyprlandCompatibility


class PreflightState(str, Enum):
    READY = "ready"
    READY_UNSUPPORTED_DISTRO = "ready_unsupported_distro"
    MISSING_DEPENDENCIES = "missing_dependencies"
    UNKNOWN_HYPRLAND = "unknown_hyprland"
    UNSUPPORTED_ENVIRONMENT = "unsupported_environment"
    SOURCE_INVALID = "source_invalid"
    INSTALL_STATE_CONFLICT = "install_state_conflict"


@dataclass(frozen=True)
class FilesystemCheck:
    path: str
    free_bytes: int | None
    writable: bool
    required_writable: bool
    detail: str


@dataclass(frozen=True)
class ManifestSnapshot:
    valid: bool
    schema_version: int | None
    release_version: str | None
    digest: str | None
    component_count: int
    dependency_count: int
    capability_count: int
    artifact_count: int
    build_unit_count: int
    error: str | None = None


@dataclass(frozen=True)
class EnvironmentSnapshot:
    architecture: str
    kernel: str
    distro: DistroInfo
    package_manager: object
    session: object
    hyprland: object
    displays: tuple[object, ...]
    capabilities: tuple[CapabilityResult, ...]
    filesystem: tuple[FilesystemCheck, ...]
    installation: InstallationState
    manifest: ManifestSnapshot
    state: PreflightState
    blockers: tuple[str, ...]
    warnings: tuple[str, ...]

    @property
    def ready(self) -> bool:
        return self.state in {PreflightState.READY, PreflightState.READY_UNSUPPORTED_DISTRO}


class PreflightScanner:
    def __init__(
        self,
        *,
        paths: XdgPaths,
        source_root: Path,
        env: Mapping[str, str] | None = None,
        runner: CommandRunner | None = None,
        os_release_text: str | None = None,
        architecture: str | None = None,
        kernel: str | None = None,
        probe_temp_root: Path | None = None,
    ) -> None:
        self.paths = paths
        self.source_root = source_root
        self.env = dict(os.environ if env is None else env)
        self.runner = runner or CommandRunner(env=self.env)
        self.os_release_text = os_release_text
        self.architecture = architecture or platform.machine() or "unknown"
        self.kernel = kernel or platform.release() or "unknown"
        self.probe_temp_root = probe_temp_root or (self.paths.installer_cache / "preflight")

    def scan(self) -> EnvironmentSnapshot:
        distro = detect_distro(text=self.os_release_text) if self.os_release_text is not None else detect_distro()
        package_manager = detect_package_manager(self.runner, distro=distro)
        session = detect_session(env=self.env, runner=self.runner)
        hyprland = detect_hyprland(self.runner)
        displays = detect_displays(self.runner, hyprland)

        registry: ManifestRegistry | None = None
        manifest_error: str | None = None
        try:
            registry = load_manifest(self.source_root / "components")
            manifest = ManifestSnapshot(
                True, registry.schema_version, registry.release_version, registry.digest,
                len(registry.components), len(registry.dependencies), len(registry.capabilities),
                len(registry.artifacts), len(registry.build_units), None,
            )
        except (ManifestError, OSError) as exc:
            manifest_error = str(exc)
            manifest = ManifestSnapshot(False, None, None, None, 0, 0, 0, 0, 0, manifest_error)

        if registry is not None:
            scanner = CapabilityScanner(
                self.runner,
                temp_root=self.probe_temp_root,
                env=self.env,
                registry=registry,
            )
            capabilities = scanner.scan_all()
        else:
            capabilities = ()
        filesystem = self._filesystem_checks()
        installation = detect_installation_state(
            paths=self.paths,
            source_root=self.source_root,
            runner=self.runner,
        )

        blockers: list[str] = []
        warnings: list[str] = []
        if manifest_error:
            blockers.append("canonical Realmheart manifest invalid: " + manifest_error)
        elif registry is not None and installation.source.version_text and installation.source.version_text != registry.release_version:
            blockers.append(
                f"canonical manifest release {registry.release_version} disagrees with CMake source version {installation.source.version_text}"
            )

        blockers.extend(installation.errors)
        warnings.extend(installation.warnings)

        if self.architecture not in {"x86_64", "amd64"}:
            warnings.append(f"architecture {self.architecture} is not a first-class tested Realmheart target; capability probes remain authoritative")
        if not session.wayland:
            blockers.append("Wayland session not detected")
        if not hyprland.command_responded:
            blockers.append("running Hyprland session not detected")
        elif hyprland.compatibility is HyprlandCompatibility.INCOMPATIBLE:
            blockers.append(f"Hyprland {hyprland.version or hyprland.raw_version or 'unknown'} is below Realmheart's supported minimum")
        elif hyprland.compatibility in {HyprlandCompatibility.UNPARSEABLE, HyprlandCompatibility.UNKNOWN}:
            warnings.append(f"Hyprland compatibility is {hyprland.compatibility.value}; explicit unsupported-mode approval will be required")
        elif hyprland.compatibility is HyprlandCompatibility.UNAVAILABLE:
            blockers.append("Hyprland unavailable")

        for check in filesystem:
            if check.required_writable and not check.writable:
                blockers.append(f"filesystem target is not writable: {check.path}")

        missing_required = required_failures(capabilities)
        missing_component = tuple(
            result for result in capabilities
            if result.requirement is RequirementLevel.COMPONENT and not result.satisfied
        )
        dependency_gaps = (*missing_required, *missing_component)
        if dependency_gaps:
            blockers.extend(f"missing dependency capability: {result.capability_id}" for result in dependency_gaps)

        soft_missing = tuple(
            result for result in capabilities
            if result.requirement is RequirementLevel.SOFT and not result.satisfied
        )
        if soft_missing:
            warnings.append(f"{len(soft_missing)} soft capability/capabilities are unavailable")

        if not package_manager.automatic_dependency_install:
            warnings.append(
                "unsupported package-provider mode: Realmheart will not install dependencies automatically on this distribution; all required/component capabilities must already pass"
            )

        foundational_blocked = (
            not session.wayland
            or not hyprland.command_responded
            or hyprland.compatibility in {HyprlandCompatibility.INCOMPATIBLE, HyprlandCompatibility.UNAVAILABLE}
        )
        manifest_source_invalid = (
            not manifest.valid
            or (manifest.release_version is not None and installation.source.version_text is not None and manifest.release_version != installation.source.version_text)
        )
        if installation.source.version is None or manifest_source_invalid:
            state = PreflightState.SOURCE_INVALID
        elif installation.origin.value != "none" and installation.mode is None:
            state = PreflightState.INSTALL_STATE_CONFLICT
        elif foundational_blocked:
            state = PreflightState.UNSUPPORTED_ENVIRONMENT
        elif hyprland.compatibility in {HyprlandCompatibility.UNKNOWN, HyprlandCompatibility.UNPARSEABLE}:
            state = PreflightState.UNKNOWN_HYPRLAND
        elif dependency_gaps or any("filesystem target" in item for item in blockers):
            state = PreflightState.MISSING_DEPENDENCIES
        elif not package_manager.automatic_dependency_install:
            state = PreflightState.READY_UNSUPPORTED_DISTRO
        else:
            state = PreflightState.READY

        return EnvironmentSnapshot(
            architecture=self.architecture,
            kernel=self.kernel,
            distro=distro,
            package_manager=package_manager,
            session=session,
            hyprland=hyprland,
            displays=displays,
            capabilities=capabilities,
            filesystem=filesystem,
            installation=installation,
            manifest=manifest,
            state=state,
            blockers=tuple(blockers),
            warnings=tuple(warnings),
        )

    def _filesystem_checks(self) -> tuple[FilesystemCheck, ...]:
        paths = (
            (self.source_root, False),
            (self.paths.config_home, True),
            (self.paths.state_home, True),
            (self.paths.data_home, True),
        )
        checks: list[FilesystemCheck] = []
        seen: set[Path] = set()
        for target, required_writable in paths:
            target = target.expanduser()
            if target in seen:
                continue
            seen.add(target)
            existing = _nearest_existing_parent(target)
            writable = bool(existing and os.access(existing, os.W_OK | os.X_OK))
            free_bytes: int | None = None
            if existing:
                try:
                    free_bytes = shutil.disk_usage(existing).free
                except OSError:
                    pass
            detail = f"checked nearest existing parent {existing}" if existing else "no existing parent found"
            checks.append(FilesystemCheck(str(target), free_bytes, writable, required_writable, detail))
        return tuple(checks)


def _nearest_existing_parent(path: Path) -> Path | None:
    candidate = path
    while True:
        if candidate.exists():
            return candidate
        if candidate.parent == candidate:
            return None
        candidate = candidate.parent
