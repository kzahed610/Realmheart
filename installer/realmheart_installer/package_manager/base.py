"""Package-manager-neutral models.

The package layer is deliberately secondary to capability probes: a package is
only a possible provider of a capability. Successful package-manager commands
never substitute for re-running the capability probe.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class ProviderResolution(str, Enum):
    SATISFIED = "satisfied"
    INSTALL = "install"
    UPGRADE = "upgrade"
    MANUAL = "manual"
    UNAVAILABLE = "unavailable"
    UNMAPPED = "unmapped"


@dataclass(frozen=True)
class PackageState:
    package: str
    installed: bool
    installed_version: str | None
    repository_available: bool
    repository: str | None = None
    repository_version: str | None = None


@dataclass(frozen=True)
class CapabilityProviderPlan:
    capability_id: str
    display_name: str
    packages: tuple[str, ...]
    resolution: ProviderResolution
    reason: str
    component: str | None = None


@dataclass(frozen=True)
class DependencyPackagePlan:
    manager: str
    providers: tuple[CapabilityProviderPlan, ...]
    packages: tuple[str, ...]
    mutation_blockers: tuple[str, ...] = ()

    @property
    def actionable(self) -> bool:
        return bool(self.packages)

    @property
    def unresolved(self) -> tuple[CapabilityProviderPlan, ...]:
        return tuple(
            item
            for item in self.providers
            if item.resolution in {
                ProviderResolution.MANUAL,
                ProviderResolution.UNAVAILABLE,
                ProviderResolution.UNMAPPED,
            }
        )


@dataclass(frozen=True)
class PackageProvenance:
    package: str
    required_by: tuple[str, ...]
    installed_before: bool
    version_before: str | None
    repository: str | None
    repository_version: str | None
    install_attempted: bool
    installed_by_transaction: bool
    changed_by_transaction: bool
    version_after: str | None
    result: str


@dataclass(frozen=True)
class PackageInstallResult:
    manager: str
    packages: tuple[str, ...]
    command: tuple[str, ...]
    returncode: int
    provenance: tuple[PackageProvenance, ...]
    error: str | None = None

    @property
    def ok(self) -> bool:
        return self.returncode == 0 and all(item.result == "pass" for item in self.provenance)


@dataclass(frozen=True)
class PackageRemovalResult:
    manager: str
    requested: tuple[str, ...]
    removed: tuple[str, ...]
    command: tuple[str, ...]
    returncode: int
    error: str | None = None

    @property
    def ok(self) -> bool:
        return self.returncode == 0 and self.error is None
