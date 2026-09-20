"""Deterministic evidence classification for component failures.

Classification maps observed check failures onto plan failure classes with
explicit confidence.  It never converts an unobserved (UNKNOWN) probe into a
known failure class, and rule order cannot change the primary evidence.
"""
from __future__ import annotations

from dataclasses import dataclass

from .health import HealthCheckResult, HealthStatus


@dataclass(frozen=True)
class FailureClassification:
    failure_class: str
    confidence: str
    evidence_ids: tuple[str, ...]


_RULES = (
    ("artifact_missing", "COMPONENT_ARTIFACT_MISSING", "HIGH"),
    ("artifact_type_mismatch", "COMPONENT_ARTIFACT_INVALID", "HIGH"),
    ("version_mismatch", "DEPENDENCY_VERSION_MISMATCH", "HIGH"),
    ("hash_mismatch", "COMPONENT_ARTIFACT_CORRUPT", "HIGH"),
)


def classify_failure(
    checks: tuple[HealthCheckResult, ...],
    *,
    missing_capabilities: tuple[str, ...] = (),
    failed_capabilities: tuple[str, ...] = (),
    failed_upstream: tuple[str, ...] = (),
) -> FailureClassification:
    """Classify one component's observed failures.

    Capability and upstream evidence participate only when it was actually
    observed failing; an unobserved condition never becomes a failure class.
    """

    observed = tuple(item for item in checks if item.status is HealthStatus.FAIL)
    ordered = tuple(sorted(observed, key=lambda item: item.check_id))
    for rule_reason, failure_class, confidence in _RULES:
        matching = tuple(item for item in ordered if item.reason_code == rule_reason)
        if matching:
            return FailureClassification(failure_class, confidence, tuple(item.check_id for item in matching))
    if missing_capabilities:
        return FailureClassification(
            "DEPENDENCY_MISSING", "HIGH", tuple(sorted(missing_capabilities))
        )
    if failed_capabilities:
        return FailureClassification(
            "DEPENDENCY_VERSION_MISMATCH", "MEDIUM", tuple(sorted(failed_capabilities))
        )
    if failed_upstream:
        return FailureClassification(
            "COMPONENT_DEPENDENCY_FAILURE", "HIGH", tuple(sorted(failed_upstream))
        )
    if not ordered:
        return FailureClassification("UNKNOWN", "LOW", ())
    return FailureClassification("OBSERVED_FAILURE", "MEDIUM", tuple(item.check_id for item in ordered))
