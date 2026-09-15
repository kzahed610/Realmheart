"""Shared, tool-independent Realmheart product metadata.

Installer and future Doctor consume this package.  It intentionally contains no
installer handlers, mutation code, or Doctor repair implementations.
"""

from .manifest import (
    ArtifactSpec,
    BuildUnitSpec,
    CapabilitySpec,
    ComponentDependencySpec,
    ComponentSpec,
    ExternalDependencySpec,
    HealthCheckSpec,
    ManifestError,
    ManifestRegistry,
    ProbeSpec,
    VersionCompatibility,
    VersionSpec,
    canonical_artifact_path_matches,
    classify_version,
    load_manifest,
    normalize_observed_artifact_path,
    resolve_canonical_artifact_path,
)

__all__ = [
    "ArtifactSpec",
    "BuildUnitSpec",
    "CapabilitySpec",
    "ComponentDependencySpec",
    "ComponentSpec",
    "ExternalDependencySpec",
    "HealthCheckSpec",
    "ManifestError",
    "ManifestRegistry",
    "ProbeSpec",
    "VersionCompatibility",
    "VersionSpec",
    "canonical_artifact_path_matches",
    "classify_version",
    "load_manifest",
    "normalize_observed_artifact_path",
    "resolve_canonical_artifact_path",
]

from .forensics import (
    ArtifactObservation,
    CapabilityObservation,
    CurrentHealthSnapshot,
    DriftKind,
    DriftRecord,
    ForensicContractError,
    ForensicIncident,
    ForensicReport,
    InstalledStateReceipt,
    ObservationOutcome,
    ReadinessState,
    analyze_forensics,
    load_health_snapshot,
    load_installed_receipt,
    parse_health_snapshot,
    parse_installed_receipt,
    serialize_health_snapshot,
    select_health_checks,
)

__all__ += [
    "ArtifactObservation", "CapabilityObservation", "CurrentHealthSnapshot",
    "DriftKind", "DriftRecord", "ForensicContractError", "ForensicIncident",
    "ForensicReport", "InstalledStateReceipt", "ObservationOutcome", "ReadinessState",
    "analyze_forensics", "load_health_snapshot", "load_installed_receipt",
    "parse_health_snapshot", "parse_installed_receipt", "serialize_health_snapshot",
    "select_health_checks",
]
