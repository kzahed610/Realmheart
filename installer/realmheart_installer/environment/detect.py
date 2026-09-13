"""Read-only host/session/Hyprland/display detection."""

from __future__ import annotations

import json
import os
import platform
from dataclasses import dataclass, field
from pathlib import Path
from typing import Mapping

from .command import CommandRunner
from .support import HyprlandCompatibility, ParsedVersion, classify_hyprland, parse_version


@dataclass(frozen=True)
class DistroInfo:
    id: str = "unknown"
    name: str = "Unknown Linux"
    pretty_name: str = "Unknown Linux"
    version_id: str | None = None
    id_like: tuple[str, ...] = ()


@dataclass(frozen=True)
class PackageManagerInfo:
    kind: str | None
    executable: str | None
    automatic_dependency_install: bool


@dataclass(frozen=True)
class SessionInfo:
    session_type: str | None
    wayland_display: str | None
    hyprland_instance_signature: str | None
    wayland: bool
    hyprland_environment: bool
    systemd_user_available: bool


@dataclass(frozen=True)
class HyprlandInfo:
    executable: str | None
    command_responded: bool
    raw_version: str | None
    version: ParsedVersion | None
    compatibility: HyprlandCompatibility
    branch: str | None = None
    commit: str | None = None
    abi_hash: str | None = None
    dirty: bool | None = None
    error: str | None = None


@dataclass(frozen=True)
class DisplayInfo:
    name: str
    description: str | None
    width: int | None
    height: int | None
    refresh_hz: float | None
    scale: float | None
    x: int | None
    y: int | None
    focused: bool
    disabled: bool


@dataclass(frozen=True)
class HostInfo:
    architecture: str
    kernel: str
    distro: DistroInfo
    package_manager: PackageManagerInfo
    session: SessionInfo
    hyprland: HyprlandInfo
    displays: tuple[DisplayInfo, ...] = field(default_factory=tuple)


def _parse_os_release_text(text: str) -> DistroInfo:
    values: dict[str, str] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in {'"', "'"}:
            value = value[1:-1]
        values[key] = value
    distro_id = values.get("ID", "unknown").lower()
    return DistroInfo(
        id=distro_id,
        name=values.get("NAME", distro_id if distro_id != "unknown" else "Unknown Linux"),
        pretty_name=values.get("PRETTY_NAME", values.get("NAME", "Unknown Linux")),
        version_id=values.get("VERSION_ID"),
        id_like=tuple(item.lower() for item in values.get("ID_LIKE", "").split() if item),
    )


def detect_distro(*, os_release_path: Path = Path("/etc/os-release"), text: str | None = None) -> DistroInfo:
    if text is None:
        try:
            text = os_release_path.read_text(encoding="utf-8")
        except OSError:
            return DistroInfo()
    return _parse_os_release_text(text)


def is_arch_family(distro: DistroInfo) -> bool:
    return distro.id == "arch" or "arch" in distro.id_like


def detect_package_manager(runner: CommandRunner, *, distro: DistroInfo | None = None) -> PackageManagerInfo:
    pacman = runner.which("pacman")
    if pacman:
        return PackageManagerInfo("pacman", pacman, bool(distro and is_arch_family(distro)))
    # These are intentionally detection-only in v1. Realmheart may still run on
    # a hand-prepared non-Arch machine when every capability already passes.
    for kind, executable in (
        ("apt", "apt-get"),
        ("dnf", "dnf"),
        ("zypper", "zypper"),
        ("xbps", "xbps-install"),
        ("apk", "apk"),
    ):
        path = runner.which(executable)
        if path:
            return PackageManagerInfo(kind, path, False)
    return PackageManagerInfo(None, None, False)


def detect_session(*, env: Mapping[str, str], runner: CommandRunner) -> SessionInfo:
    session_type = env.get("XDG_SESSION_TYPE")
    wayland_display = env.get("WAYLAND_DISPLAY")
    signature = env.get("HYPRLAND_INSTANCE_SIGNATURE")
    wayland = (session_type or "").lower() == "wayland" or bool(wayland_display)
    systemd_user = False
    systemctl = runner.which("systemctl")
    if systemctl:
        result = runner.run((systemctl, "--user", "show-environment"), timeout=3.0)
        systemd_user = result.ok
    return SessionInfo(
        session_type=session_type,
        wayland_display=wayland_display,
        hyprland_instance_signature=signature,
        wayland=wayland,
        hyprland_environment=bool(signature),
        systemd_user_available=systemd_user,
    )


def detect_hyprland(runner: CommandRunner) -> HyprlandInfo:
    executable = runner.which("hyprctl")
    if not executable:
        return HyprlandInfo(
            executable=None,
            command_responded=False,
            raw_version=None,
            version=None,
            compatibility=HyprlandCompatibility.UNAVAILABLE,
            error="hyprctl was not found in PATH",
        )

    result = runner.run((executable, "version", "-j"), timeout=4.0)
    if result.ok:
        try:
            data = json.loads(result.stdout)
        except json.JSONDecodeError:
            data = None
        if isinstance(data, dict):
            raw = str(data.get("version") or data.get("tag") or "") or None
            version = parse_version(raw)
            dirty_value = data.get("dirty")
            dirty = dirty_value if isinstance(dirty_value, bool) else None
            return HyprlandInfo(
                executable=executable,
                command_responded=True,
                raw_version=raw,
                version=version,
                compatibility=classify_hyprland(version),
                branch=str(data.get("branch")) if data.get("branch") is not None else None,
                commit=str(data.get("commit")) if data.get("commit") is not None else None,
                abi_hash=str(data.get("abiHash")) if data.get("abiHash") is not None else None,
                dirty=dirty,
            )

    fallback = runner.run((executable, "version"), timeout=4.0)
    if fallback.ok:
        raw = fallback.stdout.strip().splitlines()[0] if fallback.stdout.strip() else None
        version = parse_version(fallback.stdout)
        return HyprlandInfo(
            executable=executable,
            command_responded=True,
            raw_version=raw,
            version=version,
            compatibility=classify_hyprland(version),
        )

    reason = (result.stderr or fallback.stderr or "hyprctl did not respond").strip()
    return HyprlandInfo(
        executable=executable,
        command_responded=False,
        raw_version=None,
        version=None,
        compatibility=HyprlandCompatibility.UNAVAILABLE,
        error=reason,
    )


def detect_displays(runner: CommandRunner, hyprland: HyprlandInfo) -> tuple[DisplayInfo, ...]:
    if not hyprland.command_responded or not hyprland.executable:
        return ()
    result = runner.run((hyprland.executable, "monitors", "-j"), timeout=4.0)
    if not result.ok:
        return ()
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return ()
    if not isinstance(payload, list):
        return ()
    displays: list[DisplayInfo] = []
    for item in payload:
        if not isinstance(item, dict) or not item.get("name"):
            continue
        displays.append(
            DisplayInfo(
                name=str(item["name"]),
                description=str(item.get("description")) if item.get("description") is not None else None,
                width=_as_int(item.get("width")),
                height=_as_int(item.get("height")),
                refresh_hz=_as_float(item.get("refreshRate")),
                scale=_as_float(item.get("scale")),
                x=_as_int(item.get("x")),
                y=_as_int(item.get("y")),
                focused=bool(item.get("focused", False)),
                disabled=bool(item.get("disabled", False)),
            )
        )
    return tuple(displays)


def detect_host(*, env: Mapping[str, str] | None = None, runner: CommandRunner | None = None) -> HostInfo:
    environ = dict(os.environ if env is None else env)
    active_runner = runner or CommandRunner(env=environ)
    distro = detect_distro()
    package_manager = detect_package_manager(active_runner, distro=distro)
    session = detect_session(env=environ, runner=active_runner)
    hyprland = detect_hyprland(active_runner)
    displays = detect_displays(active_runner, hyprland)
    return HostInfo(
        architecture=platform.machine() or "unknown",
        kernel=platform.release() or "unknown",
        distro=distro,
        package_manager=package_manager,
        session=session,
        hyprland=hyprland,
        displays=displays,
    )


def _as_int(value: object) -> int | None:
    try:
        return int(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return None


def _as_float(value: object) -> float | None:
    try:
        return float(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return None
