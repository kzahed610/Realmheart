"""Realmheart source identity, installed-origin and install-mode detection.

Phase 6 remains observational. It reads the canonical CMake project version,
managed installed-state receipt, legacy user service metadata, and known local
build locations. No user config is executed and no installation state is
modified.
"""

from __future__ import annotations

import json
import re
import shlex
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

from ..constants import INSTALLED_STATE_SCHEMA_VERSION
from ..context import XdgPaths
from ..models import InstallMode
from .command import CommandRunner
from .support import ParsedVersion, parse_version


_PROJECT_VERSION_RE = re.compile(
    r"project\s*\(\s*Realmheart\s+VERSION\s+([0-9]+(?:\.[0-9]+){1,2})\b",
    re.IGNORECASE | re.MULTILINE,
)


class InstallOrigin(str, Enum):
    NONE = "none"
    LEGACY_SCRIPT = "legacy_script"
    MANAGED_INSTALLER = "managed_installer"
    DEVELOPMENT = "development"


class VersionEvidence(str, Enum):
    NONE = "none"
    RECEIPT = "receipt"
    BINARY = "binary"
    SOURCE_CHECKOUT = "source_checkout"


@dataclass(frozen=True)
class SourceIdentity:
    source_root: str
    version: ParsedVersion | None
    version_text: str | None
    cmake_path: str
    git_commit: str | None
    git_dirty: bool | None
    error: str | None = None


@dataclass(frozen=True)
class InstallationState:
    origin: InstallOrigin
    source: SourceIdentity
    installed_version: ParsedVersion | None
    installed_version_text: str | None
    version_evidence: VersionEvidence
    mode: InstallMode | None
    receipt_path: str | None
    service_path: str | None
    service_exec_start: str | None
    binary_path: str | None
    requires_pre_adoption_snapshot: bool
    errors: tuple[str, ...]
    warnings: tuple[str, ...]

    @property
    def target_version(self) -> ParsedVersion | None:
        return self.source.version

    @property
    def complete(self) -> bool:
        return self.source.version is not None and self.mode is not None and not self.errors


def detect_source_identity(source_root: Path, runner: CommandRunner) -> SourceIdentity:
    source_root = source_root.resolve()
    cmake_path = source_root / "CMakeLists.txt"
    try:
        text = cmake_path.read_text(encoding="utf-8")
    except OSError as exc:
        return SourceIdentity(
            source_root=str(source_root),
            version=None,
            version_text=None,
            cmake_path=str(cmake_path),
            git_commit=None,
            git_dirty=None,
            error=f"cannot read canonical CMake project identity: {exc}",
        )

    match = _PROJECT_VERSION_RE.search(text)
    if not match:
        return SourceIdentity(
            source_root=str(source_root),
            version=None,
            version_text=None,
            cmake_path=str(cmake_path),
            git_commit=None,
            git_dirty=None,
            error="CMakeLists.txt does not declare project(Realmheart VERSION ...)",
        )

    version_text = match.group(1)
    version = parse_version(version_text)
    if version is None:
        return SourceIdentity(
            source_root=str(source_root),
            version=None,
            version_text=version_text,
            cmake_path=str(cmake_path),
            git_commit=None,
            git_dirty=None,
            error=f"canonical Realmheart version is unparseable: {version_text}",
        )

    git_commit: str | None = None
    git_dirty: bool | None = None
    git = runner.which("git")
    if git and (source_root / ".git").exists():
        commit = runner.run((git, "-C", str(source_root), "rev-parse", "HEAD"), timeout=4.0)
        if commit.ok and commit.stdout.strip():
            git_commit = commit.stdout.strip().splitlines()[0]
        status = runner.run(
            (git, "-C", str(source_root), "status", "--porcelain", "--untracked-files=normal"),
            timeout=5.0,
        )
        if status.ok:
            git_dirty = bool(status.stdout.strip())

    return SourceIdentity(
        source_root=str(source_root),
        version=version,
        version_text=version_text,
        cmake_path=str(cmake_path),
        git_commit=git_commit,
        git_dirty=git_dirty,
    )


def detect_installation_state(
    *,
    paths: XdgPaths,
    source_root: Path,
    runner: CommandRunner,
) -> InstallationState:
    source = detect_source_identity(source_root, runner)
    warnings: list[str] = []
    errors: list[str] = []
    if source.error:
        errors.append(source.error)
    if source.git_dirty:
        warnings.append("Realmheart source checkout has tracked or untracked modifications; target build provenance will be dirty")

    receipt_path = paths.realmheart_state / "installed-state.json"
    service_path = paths.config_home / "systemd/user/realmheart.service"

    installed_version: ParsedVersion | None = None
    installed_text: str | None = None
    evidence = VersionEvidence.NONE
    origin = InstallOrigin.NONE
    binary_path: Path | None = None
    service_exec_start: str | None = None

    if receipt_path.exists():
        origin = InstallOrigin.MANAGED_INSTALLER
        try:
            payload = json.loads(receipt_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            errors.append(f"managed installed-state receipt is unreadable/corrupt: {exc}")
        else:
            if not isinstance(payload, dict):
                errors.append("managed installed-state receipt is not a JSON object")
            else:
                schema_version = payload.get("schema_version")
                if not isinstance(schema_version, int) or isinstance(schema_version, bool):
                    errors.append("managed installed-state receipt has no valid integer schema_version")
                elif schema_version < 1 or schema_version > INSTALLED_STATE_SCHEMA_VERSION:
                    errors.append(
                        f"managed installed-state receipt schema {schema_version} is unsupported "
                        f"(max supported {INSTALLED_STATE_SCHEMA_VERSION})"
                    )
                disposition = payload.get("disposition")
                if disposition is not None and disposition != "kept":
                    errors.append(
                        f"current installed-state receipt has unexpected disposition {disposition!r}; "
                        "a rolled-back target must not replace the kept receipt"
                    )
                raw = payload.get("realmheart_version")
                if not isinstance(raw, str) or not raw.strip():
                    errors.append("managed installed-state receipt has no realmheart_version")
                else:
                    installed_text = raw.strip()
                    installed_version = parse_version(installed_text)
                    if installed_version is None:
                        errors.append(f"managed installed-state receipt has unparseable realmheart_version: {installed_text}")
                    elif not errors:
                        evidence = VersionEvidence.RECEIPT
    elif service_path.exists():
        origin = InstallOrigin.LEGACY_SCRIPT
        service_exec_start, binary_path = _read_service_exec(service_path, paths.home)
        if service_exec_start is None:
            warnings.append("legacy Realmheart user service exists but ExecStart could not be resolved")
        installed_version, installed_text, evidence = _version_from_binary_or_source(
            binary_path=binary_path,
            source=source,
            source_root=source_root,
            runner=runner,
        )
        if installed_version is None:
            errors.append(
                "legacy Realmheart installation detected but its installed version cannot be proven; "
                "upgrade/downgrade mode classification is unsafe"
            )
    else:
        development_binary = _find_development_binary(source_root, runner)
        if development_binary is not None:
            origin = InstallOrigin.DEVELOPMENT
            binary_path = development_binary
            installed_version, installed_text, evidence = _version_from_binary_or_source(
                binary_path=development_binary,
                source=source,
                source_root=source_root,
                runner=runner,
            )
            if installed_version is None:
                errors.append(
                    "unmanaged/development Realmheart binary detected but its version cannot be proven; "
                    "install mode classification is unsafe"
                )

    mode = None if errors else _resolve_mode(origin, installed_version, source.version)
    if origin is not InstallOrigin.NONE and mode is None and not errors:
        errors.append("existing Realmheart installation could not be classified as reinstall/upgrade/downgrade")

    requires_pre_adoption = origin in {InstallOrigin.LEGACY_SCRIPT, InstallOrigin.DEVELOPMENT}
    if requires_pre_adoption:
        warnings.append(
            "unmanaged Realmheart state will require a pre-adoption snapshot; historical .bak files are not assumed pristine"
        )

    return InstallationState(
        origin=origin,
        source=source,
        installed_version=installed_version,
        installed_version_text=installed_text,
        version_evidence=evidence,
        mode=mode,
        receipt_path=str(receipt_path) if receipt_path.exists() else None,
        service_path=str(service_path) if service_path.exists() else None,
        service_exec_start=service_exec_start,
        binary_path=str(binary_path) if binary_path is not None else None,
        requires_pre_adoption_snapshot=requires_pre_adoption,
        errors=tuple(errors),
        warnings=tuple(warnings),
    )


def _resolve_mode(
    origin: InstallOrigin,
    installed: ParsedVersion | None,
    target: ParsedVersion | None,
) -> InstallMode | None:
    if target is None:
        return None
    if origin is InstallOrigin.NONE:
        return InstallMode.FRESH
    if installed is None:
        return None
    if installed.tuple == target.tuple:
        return InstallMode.REINSTALL
    if installed.tuple < target.tuple:
        return InstallMode.UPGRADE
    return InstallMode.DOWNGRADE


def _read_service_exec(service_path: Path, home: Path) -> tuple[str | None, Path | None]:
    try:
        lines = service_path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None, None
    for line in lines:
        stripped = line.strip()
        if not stripped.startswith("ExecStart="):
            continue
        raw = stripped.split("=", 1)[1].strip()
        if not raw:
            return raw, None
        try:
            parts = shlex.split(raw, posix=True)
        except ValueError:
            return raw, None
        if not parts:
            return raw, None
        token = parts[0].lstrip("-+!@:")
        token = token.replace("%h", str(home))
        path = Path(token).expanduser()
        return raw, path if path.is_absolute() else None
    return None, None


def _version_from_binary_or_source(
    *,
    binary_path: Path | None,
    source: SourceIdentity,
    source_root: Path,
    runner: CommandRunner,
) -> tuple[ParsedVersion | None, str | None, VersionEvidence]:
    if binary_path is not None and binary_path.is_file():
        result = runner.run((str(binary_path), "--version"), timeout=4.0)
        if result.ok:
            combined = (result.stdout or result.stderr).strip()
            parsed = parse_version(combined)
            if parsed is not None:
                return parsed, str(parsed), VersionEvidence.BINARY

    if binary_path is not None and source.version is not None and _is_within(binary_path, source_root):
        return source.version, source.version_text, VersionEvidence.SOURCE_CHECKOUT
    return None, None, VersionEvidence.NONE


def _find_development_binary(source_root: Path, runner: CommandRunner) -> Path | None:
    candidates = (
        source_root / "build-hybrid/realmheart",
        source_root / "build/realmheart",
        source_root / "build-release/realmheart",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    path = runner.which("realmheart")
    return Path(path) if path else None


def _is_within(path: Path, parent: Path) -> bool:
    try:
        path.resolve(strict=False).relative_to(parent.resolve())
        return True
    except ValueError:
        return False
