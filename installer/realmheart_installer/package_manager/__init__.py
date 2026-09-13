"""Package-manager abstraction for Realmheart dependency acquisition."""

from .base import (
    DependencyPackagePlan,
    PackageInstallResult,
    PackageRemovalResult,
    PackageProvenance,
    PackageState,
    ProviderResolution,
)
from .pacman import PacmanAdapter, build_pacman_dependency_plan

__all__ = [
    "DependencyPackagePlan",
    "PackageInstallResult",
    "PackageRemovalResult",
    "PackageProvenance",
    "PackageState",
    "ProviderResolution",
    "PacmanAdapter",
    "build_pacman_dependency_plan",
]
