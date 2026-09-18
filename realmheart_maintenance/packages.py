"""Tool-independent package provenance vocabulary and Arch provider mapping.

Package *names* and the dependency-to-provider mapping are shared product data
so the installer and the read-only Doctor resolve the same dependencies to the
same packages.  This module performs no mutation; the query helper runs a
structured, bounded, privilege-free ``pacman -Q`` only.
"""
from __future__ import annotations

import re
import shutil
import subprocess
from dataclasses import dataclass
from typing import Callable, Sequence


@dataclass(frozen=True)
class PackageProvenanceRecord:
    dependency_id: str
    package: str
    installed_before: bool
    version_before: str | None
    installed_by_transaction: bool
    version_after: str | None
    install_result: str


@dataclass(frozen=True)
class PacmanProvider:
    packages: tuple[str, ...]
    automatic: bool = True
    note: str | None = None


# Verified Arch/CachyOS provider mapping keyed by logical dependency ID.
# Capability ownership and lifecycle live in components/*.toml; this table only
# knows how Arch spells providers for those logical dependencies.
PACMAN_DEPENDENCY_PROVIDERS = {
    'dep.build.cmake': PacmanProvider(('cmake',)),
    'dep.build.ninja': PacmanProvider(('ninja',)),
    'dep.build.pkg-config': PacmanProvider(('pkgconf',)),
    'dep.runtime.python3': PacmanProvider(('python',)),
    'dep.runtime.bash': PacmanProvider(('bash',)),
    'dep.runtime.hyprctl': PacmanProvider(('hyprland',), automatic=False, note='Realmheart does not install or replace Hyprland'),
    'dep.runtime.systemctl': PacmanProvider(('systemd',), automatic=False, note='Realmheart requires an already usable systemd user session'),
    'dep.runtime.systemd-inhibit': PacmanProvider(('systemd',), automatic=False, note='requires the host systemd stack'),
    'dep.runtime.loginctl': PacmanProvider(('systemd',), automatic=False, note='requires the host systemd/logind stack'),
    'dep.runtime.wpctl': PacmanProvider(('wireplumber',)),
    'dep.runtime.pactl': PacmanProvider(('libpulse',)),
    'dep.runtime.brightnessctl': PacmanProvider(('brightnessctl',)),
    'dep.runtime.hypridle': PacmanProvider(('hypridle',)),
    'dep.runtime.hyprsunset': PacmanProvider(('hyprsunset',)),
    'dep.runtime.hyprlock': PacmanProvider(('hyprlock',)),
    'dep.runtime.matugen': PacmanProvider(('matugen',)),
    'dep.wl-clipboard': PacmanProvider(('wl-clipboard',)),
    'dep.runtime.cliphist': PacmanProvider(('cliphist',)),
    'dep.runtime.tesseract': PacmanProvider(('tesseract',)),
    'dep.runtime.wf-recorder': PacmanProvider(('wf-recorder',)),
    'dep.runtime.grim': PacmanProvider(('grim',)),
    'dep.runtime.slurp': PacmanProvider(('slurp',)),
    'dep.runtime.curl': PacmanProvider(('curl',)),
    'dep.runtime.notify-send': PacmanProvider(('libnotify',)),
    'dep.runtime.kitty': PacmanProvider(('kitty',)),
    'dep.runtime.fish': PacmanProvider(('fish',)),
    'dep.runtime.starship': PacmanProvider(('starship',)),
    'dep.runtime.xdg-user-dir': PacmanProvider(('xdg-user-dirs',)),
    'dep.runtime.pidof': PacmanProvider(('procps-ng',)),
    'dep.runtime.playerctl': PacmanProvider(('playerctl',)),
    'dep.runtime.bc': PacmanProvider(('bc',)),
    'dep.runtime.find': PacmanProvider(('findutils',)),
    'dep.runtime.shuf': PacmanProvider(('coreutils',)),
    'dep.runtime.xargs': PacmanProvider(('findutils',)),
    'dep.dbus': PacmanProvider(('dbus',)),
    'dep.lib.gio': PacmanProvider(('glib2',)),
    'dep.lib.gtk4': PacmanProvider(('gtk4',)),
    'dep.lib.gtk4-layer-shell': PacmanProvider(('gtk4-layer-shell',)),
    'dep.lib.epoxy': PacmanProvider(('libepoxy',)),
    'dep.lib.gdk-pixbuf': PacmanProvider(('gdk-pixbuf2',)),
    'dep.lib.jpeg': PacmanProvider(('libjpeg-turbo',)),
    'dep.lib.sqlite3': PacmanProvider(('sqlite',)),
    'dep.hyprland.devel': PacmanProvider(
        ('hyprland',), automatic=False,
        note="Realmheart does not install or replace Hyprland; the development API must match the user's compositor",
    ),
    'dep.lib.glesv2': PacmanProvider(('libglvnd',)),
    'dep.wayland.client': PacmanProvider(('wayland',)),
    'dep.wayland.wlr-protocols': PacmanProvider(('wlr-protocols',)),
    'dep.wallpaper.wayland-egl': PacmanProvider(('wayland',)),
    'dep.wallpaper.egl': PacmanProvider(('libglvnd',)),
    'dep.wallpaper.wayland-protocols': PacmanProvider(('wayland-protocols',)),
    'dep.wayland.scanner': PacmanProvider(('wayland',)),
    'dep.build.cxx26': PacmanProvider(('gcc',)),
    'dep.opencv.ximgproc': PacmanProvider(('opencv',)),
    'dep.pam.devel': PacmanProvider(('pam',)),
    'dep.verification.gtest': PacmanProvider(('gtest',)),
    'dep.tesseract.lang.eng': PacmanProvider(('tesseract-data-eng',)),
    'dep.runtime.nmcli': PacmanProvider(('networkmanager',), note="package install does not enable or replace the user's networking backend"),
    'dep.runtime.bluetoothctl': PacmanProvider(('bluez', 'bluez-utils'), note='package install does not synthesize Bluetooth hardware'),
    'dep.runtime.powerprofilesctl': PacmanProvider(('power-profiles-daemon',)),
    'dep.runtime.systemd-user': PacmanProvider(('systemd',), automatic=False, note='Realmheart will not replace the host init/session stack'),
    'dep.runtime.portal-hyprland': PacmanProvider(('xdg-desktop-portal-hyprland',)),
    'dep.runtime.lens-url-opener': PacmanProvider(('xdg-utils',)),
    'dep.runtime.fx-loader-tools': PacmanProvider(('grep', 'coreutils')),
    'dep.install.privileged-file-tools': PacmanProvider(('coreutils',)),
}


_PACKAGE_NAME_RE = re.compile(r"^[A-Za-z0-9@._+:-]+$")
_QUERY_FAILURES = (OSError, subprocess.SubprocessError)


@dataclass(frozen=True)
class PackageQueryResult:
    package: str
    installed: bool
    version: str | None = None
    error: str | None = None


def _default_query_runner(argv: tuple[str, ...], timeout: float) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, capture_output=True, text=True, timeout=timeout, check=False)


def query_installed_packages(
    packages: Sequence[str],
    *,
    runner: Callable[[tuple[str, ...], float], subprocess.CompletedProcess[str]] | None = None,
    pacman: str | None = None,
    timeout: float = 8.0,
) -> tuple[PackageQueryResult, ...]:
    """Query installed packages read-only with one bounded structured argv each.

    Returns no result claiming absence when the query itself failed: a failed
    observation is reported as ``error`` rather than "not installed".
    """

    executable = pacman or shutil.which("pacman") or "pacman"
    run = runner or _default_query_runner
    results: list[PackageQueryResult] = []
    for package in packages:
        if not isinstance(package, str) or not _PACKAGE_NAME_RE.fullmatch(package):
            results.append(PackageQueryResult(package, False, error="invalid_package_name"))
            continue
        try:
            completed = run((executable, "-Q", "--", package), timeout)
        except _QUERY_FAILURES as exc:
            results.append(PackageQueryResult(package, False, error=type(exc).__name__))
            continue
        if completed.returncode != 0:
            results.append(PackageQueryResult(package, False))
            continue
        parts = (completed.stdout or "").split()
        results.append(PackageQueryResult(package, True, version=parts[1] if len(parts) >= 2 else None))
    return tuple(results)
