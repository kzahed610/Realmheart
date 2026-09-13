"""Arch/pacman dependency provider adapter.

Only configured pacman sync repositories are consulted. The adapter never uses
AUR helpers, downloads arbitrary binaries, refreshes databases with ``-Sy``, or
installs/replaces Hyprland itself.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
from typing import Iterable, Mapping

from realmheart_maintenance.manifest import ManifestRegistry, load_manifest

from ..environment.capabilities import CapabilityResult, RequirementLevel
from ..environment.command import CommandRunner
from .base import (
    CapabilityProviderPlan,
    DependencyPackagePlan,
    PackageInstallResult,
    PackageRemovalResult,
    PackageProvenance,
    PackageState,
    ProviderResolution,
)


@dataclass(frozen=True)
class PacmanProvider:
    packages: tuple[str, ...]
    automatic: bool = True
    note: str | None = None


@dataclass(frozen=True)
class PacmanUpgradeCheck:
    ok: bool
    pending: tuple[str, ...]
    error: str | None = None


# Verified Arch/CachyOS provider mapping keyed by logical dependency ID.
# Capability ownership and lifecycle live in components/*.toml; the adapter only
# knows how Arch spells providers for those logical dependencies.
PACMAN_DEPENDENCY_PROVIDERS: Mapping[str, PacmanProvider] = {
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
    'dep.hyprland.devel': PacmanProvider(('hyprland',), automatic=False, note="Realmheart does not install or replace Hyprland; the development API must match the user's compositor"),
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


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def _default_registry() -> ManifestRegistry:
    return load_manifest(_repo_root() / "components")


def _capability_provider_view(registry: ManifestRegistry | None = None) -> dict[str, PacmanProvider]:
    registry = registry or _default_registry()
    return {
        capability.id: PACMAN_DEPENDENCY_PROVIDERS[capability.dependency_id]
        for capability in registry.capabilities.values()
        if capability.dependency_id in PACMAN_DEPENDENCY_PROVIDERS
    }


# Backward-compatible generated view.  This is not independent source-of-truth.
try:
    PACMAN_CAPABILITY_PROVIDERS: Mapping[str, PacmanProvider] = _capability_provider_view()
except Exception:
    PACMAN_CAPABILITY_PROVIDERS = {}


class PacmanAdapter:
    def __init__(self, runner: CommandRunner, *, pacman: str | None = None, sudo: str | None = None) -> None:
        self.runner = runner
        self.pacman = pacman or runner.which("pacman") or "pacman"
        self.sudo = sudo if sudo is not None else runner.which("sudo")

    @property
    def name(self) -> str:
        return "pacman"

    @staticmethod
    def _validate_package_name(package: str) -> None:
        if not re.fullmatch(r"[A-Za-z0-9@._+:-]+", package):
            raise ValueError(f"invalid pacman package name: {package!r}")

    def pending_upgrades(self) -> PacmanUpgradeCheck:
        """Inspect the current sync DB without refreshing it.

        Realmheart never runs ``pacman -Sy``. A non-empty result means selected
        dependency installation would be a partial upgrade. An unreadable sync
        state also fails closed rather than guessing that package mutation is safe.
        """
        result = self.runner.run((self.pacman, "-Qu"), timeout=20.0, env={"LC_ALL": "C"})
        if result.returncode not in {0, 1}:
            detail = (result.stderr or result.stdout or f"pacman -Qu exited {result.returncode}").strip()
            return PacmanUpgradeCheck(False, (), detail)
        pending = tuple(line.strip() for line in result.stdout.splitlines() if line.strip())
        return PacmanUpgradeCheck(True, pending)

    def query(self, package: str) -> PackageState:
        self._validate_package_name(package)
        installed = self.runner.run((self.pacman, "-Q", "--", package), timeout=8.0, env={"LC_ALL": "C"})
        installed_version = self._query_version_line(installed.stdout, package) if installed.ok else None

        sync = self.runner.run((self.pacman, "-Si", "--", package), timeout=12.0, env={"LC_ALL": "C"})
        fields = self._parse_info(sync.stdout) if sync.ok else {}
        return PackageState(
            package=package,
            installed=installed.ok,
            installed_version=installed_version,
            repository_available=sync.ok,
            repository=fields.get("Repository"),
            repository_version=fields.get("Version"),
        )

    def version_of(self, package: str) -> str | None:
        return self.query(package).installed_version

    def compare_versions(self, left: str, right: str) -> int | None:
        vercmp = self.runner.which("vercmp")
        if not vercmp:
            return None
        result = self.runner.run((vercmp, left, right), timeout=4.0, env={"LC_ALL": "C"})
        if not result.ok:
            return None
        try:
            value = int(result.stdout.strip())
        except ValueError:
            return None
        return -1 if value < 0 else (1 if value > 0 else 0)

    def install(self, packages: Iterable[str], *, required_by: Mapping[str, tuple[str, ...]] | None = None) -> PackageInstallResult:
        requested = tuple(dict.fromkeys(package for package in packages if package))
        for package in requested:
            self._validate_package_name(package)
        required = required_by or {}
        before = {package: self.query(package) for package in requested}
        if not requested:
            return PackageInstallResult(self.name, (), (), 0, ())
        upgrade_check = self.pending_upgrades()
        if not upgrade_check.ok or upgrade_check.pending:
            provenance = tuple(
                self._provenance(package, required.get(package, ()), before[package], before[package], attempted=False, command_ok=False)
                for package in requested
            )
            error = (
                "unable to verify pacman upgrade safety: " + (upgrade_check.error or "unknown pacman query failure")
                if not upgrade_check.ok
                else "pacman reports pending system upgrades; Realmheart refuses a partial upgrade. Run your normal full system upgrade (pacman -Syu), then rerun the installer"
            )
            return PackageInstallResult(self.name, requested, (), 75, provenance, error=error)
        if not self.sudo:
            provenance = tuple(
                self._provenance(package, required.get(package, ()), before[package], before[package], attempted=False, command_ok=False)
                for package in requested
            )
            return PackageInstallResult(self.name, requested, (), 127, provenance, error="sudo not found; package installation requires narrow elevation")

        command = (self.sudo, self.pacman, "-S", "--needed", "--", *requested)
        result = self.runner.run(command, timeout=None, interactive=True)
        after = {package: self.query(package) for package in requested}
        provenance = tuple(
            self._provenance(package, required.get(package, ()), before[package], after[package], attempted=True, command_ok=result.ok)
            for package in requested
        )
        error = None if result.ok else (result.stderr.strip() or f"pacman exited with status {result.returncode}")
        return PackageInstallResult(self.name, requested, command, result.returncode, provenance, error=error)

    def can_remove_safely(self, package: str) -> bool | None:
        # Package-manager state alone cannot prove user intent.  Phase 17 first
        # requires installer provenance *and* an explicit uninstall cleanup
        # choice; pacman then gets an exact non-recursive removal request.
        return None

    def remove_exact(self, packages: Iterable[str]) -> PackageRemovalResult:
        """Attempt conservative exact removal of explicitly approved packages.

        Realmheart deliberately uses ``pacman -R`` rather than ``-Rs``/``-Rns``:
        pacman may refuse removal when another installed package depends on a
        candidate, but Realmheart will never recursively expand cleanup into an
        unreviewed dependency set.  Callers must already have proven the package
        was installed by the Realmheart transaction and obtained user consent.
        """

        requested = tuple(dict.fromkeys(package for package in packages if package))
        for package in requested:
            self._validate_package_name(package)
        installed = tuple(package for package in requested if self.query(package).installed)
        if not installed:
            return PackageRemovalResult(self.name, requested, (), (), 0, None)
        if not self.sudo:
            return PackageRemovalResult(self.name, requested, (), (), 127, "sudo not found; optional package cleanup requires narrow elevation")

        command = (self.sudo, self.pacman, "-R", "--", *installed)
        result = self.runner.run(command, timeout=None, interactive=True)
        removed = tuple(package for package in installed if not self.query(package).installed)
        error = None if result.ok and len(removed) == len(installed) else (
            result.stderr.strip() or
            "pacman refused or only partially completed exact package removal; remaining packages were retained"
        )
        return PackageRemovalResult(self.name, requested, removed, command, result.returncode, error)

    @staticmethod
    def _query_version_line(stdout: str, package: str) -> str | None:
        line = stdout.strip().splitlines()[0] if stdout.strip() else ""
        if not line:
            return None
        prefix = package + " "
        return line[len(prefix):].strip() if line.startswith(prefix) else None

    @staticmethod
    def _parse_info(stdout: str) -> dict[str, str]:
        fields: dict[str, str] = {}
        for line in stdout.splitlines():
            if ":" not in line:
                continue
            key, value = line.split(":", 1)
            fields[key.strip()] = value.strip()
        return fields

    @staticmethod
    def _provenance(
        package: str,
        required_by: tuple[str, ...],
        before: PackageState,
        after: PackageState,
        *,
        attempted: bool,
        command_ok: bool,
    ) -> PackageProvenance:
        installed_by = attempted and not before.installed and after.installed
        changed = attempted and before.installed_version != after.installed_version
        result = "pass" if command_ok and after.installed else "failed"
        return PackageProvenance(
            package=package,
            required_by=required_by,
            installed_before=before.installed,
            version_before=before.installed_version,
            repository=before.repository or after.repository,
            repository_version=before.repository_version or after.repository_version,
            install_attempted=attempted,
            installed_by_transaction=installed_by,
            changed_by_transaction=changed,
            version_after=after.installed_version,
            result=result,
        )


def build_pacman_dependency_plan(
    capabilities: Iterable[CapabilityResult],
    adapter: PacmanAdapter,
    *,
    registry: ManifestRegistry | None = None,
) -> DependencyPackagePlan:
    """Resolve unsatisfied required/component capabilities to pacman packages.

    Soft capabilities are intentionally omitted from the mandatory acquisition
    plan. If a provider package is already installed and no newer sync version is
    available, reinstalling it is not treated as a repair: the capability needs
    manual/runtime remediation instead.
    """

    registry = registry or _default_registry()
    providers: list[CapabilityProviderPlan] = []
    package_reasons: dict[str, set[str]] = {}

    for capability in capabilities:
        if capability.satisfied or capability.requirement is RequirementLevel.SOFT:
            continue
        canonical = registry.capabilities.get(capability.capability_id)
        provider = PACMAN_DEPENDENCY_PROVIDERS.get(canonical.dependency_id) if canonical is not None else None
        if provider is None:
            providers.append(CapabilityProviderPlan(
                capability.capability_id,
                capability.display_name,
                (),
                ProviderResolution.UNMAPPED,
                "no verified pacman provider mapping exists",
                capability.component,
            ))
            continue
        if not provider.automatic:
            providers.append(CapabilityProviderPlan(
                capability.capability_id,
                capability.display_name,
                provider.packages,
                ProviderResolution.MANUAL,
                provider.note or "automatic package mutation is disabled for this capability",
                capability.component,
            ))
            continue

        states = tuple(adapter.query(package) for package in provider.packages)
        unavailable = tuple(state.package for state in states if not state.installed and not state.repository_available)
        if unavailable:
            providers.append(CapabilityProviderPlan(
                capability.capability_id,
                capability.display_name,
                provider.packages,
                ProviderResolution.UNAVAILABLE,
                "provider package(s) unavailable in configured pacman repositories: " + ", ".join(unavailable),
                capability.component,
            ))
            continue

        candidates: list[str] = []
        upgrade = False
        for state in states:
            if not state.installed:
                candidates.append(state.package)
                continue
            if state.repository_available and state.installed_version and state.repository_version:
                comparison = adapter.compare_versions(state.installed_version, state.repository_version)
                if comparison is not None and comparison < 0:
                    candidates.append(state.package)
                    upgrade = True

        if not candidates:
            detail = "provider package(s) already installed; package reinstall is not considered a capability repair"
            if provider.note:
                detail += "; " + provider.note
            providers.append(CapabilityProviderPlan(
                capability.capability_id,
                capability.display_name,
                provider.packages,
                ProviderResolution.MANUAL,
                detail,
                capability.component,
            ))
            continue

        for package in candidates:
            package_reasons.setdefault(package, set()).add(capability.capability_id)
        detail = "verified pacman provider(s): " + ", ".join(candidates)
        if provider.note:
            detail += "; " + provider.note
        providers.append(CapabilityProviderPlan(
            capability.capability_id,
            capability.display_name,
            tuple(candidates),
            ProviderResolution.UPGRADE if upgrade else ProviderResolution.INSTALL,
            detail,
            capability.component,
        ))

    packages = tuple(sorted(package_reasons))
    blockers: tuple[str, ...] = ()
    if packages:
        upgrade_check = adapter.pending_upgrades()
        if not upgrade_check.ok:
            blockers = ("unable to verify pacman upgrade safety: " + (upgrade_check.error or "unknown pacman query failure"),)
        elif upgrade_check.pending:
            blockers = (
                "pacman reports pending system upgrades; installing selected Realmheart dependencies would be a partial upgrade. Complete a normal pacman -Syu first",
            )
    return DependencyPackagePlan("pacman", tuple(providers), packages, blockers)


def package_required_by(plan: DependencyPackagePlan) -> dict[str, tuple[str, ...]]:
    result: dict[str, set[str]] = {}
    for provider in plan.providers:
        if provider.resolution not in {ProviderResolution.INSTALL, ProviderResolution.UPGRADE}:
            continue
        for package in provider.packages:
            result.setdefault(package, set()).add(provider.capability_id)
    return {package: tuple(sorted(capabilities)) for package, capabilities in result.items()}
