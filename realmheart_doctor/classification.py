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

def classify_component(component) -> FailureClassification:
    """Classify a live ``ComponentDiagnosis`` without discarding evidence.

    The helper intentionally uses the diagnosis object's public evidence fields
    rather than probing again, so ``doctor --explain`` and persisted incidents
    describe the same observation that produced the health state.
    """

    capabilities = tuple(getattr(component, "capabilities", ()) or ())
    missing = tuple(sorted(
        item.capability_id for item in capabilities
        if getattr(item, "blocking_failure", False) and getattr(item, "state", None) == "missing"
    ))
    failed = tuple(sorted(
        item.capability_id for item in capabilities
        if getattr(item, "blocking_failure", False) and getattr(item, "state", None) == "failed"
    ))
    upstream: set[str] = set()
    for entry in tuple(getattr(component, "uncertainties", ()) or ()):
        if isinstance(entry, str) and entry.startswith("upstream_component_failed:"):
            upstream.update(part for part in entry.split(":", 1)[1].split(",") if part)
    return classify_failure(
        tuple(getattr(component, "checks", ()) or ()),
        missing_capabilities=missing,
        failed_capabilities=failed,
        failed_upstream=tuple(sorted(upstream)),
    )
