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


def _has_version_evidence(item) -> bool:
    """Whether a failed capability carries an actual observed version.

    A capability that fails with a concrete version violates the compatibility
    contract.  A capability that fails without one (a backend that is installed
    but unreachable, a version probe that could not be queried) is a different
    condition and must not be reported as a version mismatch.
    """

    version = getattr(item, "version", None)
    return isinstance(version, str) and bool(version.strip())


def classify_failure(
    checks: tuple[HealthCheckResult, ...],
    *,
    missing_capabilities: tuple[str, ...] = (),
    failed_capabilities: tuple[str, ...] = (),
    failed_upstream: tuple[str, ...] = (),
    unavailable_capabilities: tuple[str, ...] = (),
) -> FailureClassification:
    """Classify one component's observed failures.

    Capability and upstream evidence participate only when it was actually
    observed failing; an unobserved condition never becomes a failure class.
    ``failed_capabilities`` keeps its historical meaning (any failed capability)
    so callers that only know "something failed" still classify; a version
    class is chosen only when version evidence or an explicit unavailability
    split is supplied.
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
    unavailable = tuple(sorted(set(unavailable_capabilities)))
    mismatched = tuple(sorted(
        set(failed_capabilities) - set(unavailable_capabilities)
    ))
    if mismatched:
        return FailureClassification(
            "DEPENDENCY_VERSION_MISMATCH", "MEDIUM", mismatched
        )
    if unavailable:
        return FailureClassification(
            "DEPENDENCY_UNAVAILABLE", "MEDIUM", unavailable
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
    failed_items = tuple(
        item for item in capabilities
        if getattr(item, "blocking_failure", False) and getattr(item, "state", None) == "failed"
    )
    # A failed capability only counts as a version mismatch when it actually
    # produced version evidence.  "Backend unreachable" and "could not query
    # the version" are availability failures, not contract violations, and
    # reporting them as mismatches sends the reader after the wrong fix.
    mismatched = tuple(sorted(
        item.capability_id for item in failed_items if _has_version_evidence(item)
    ))
    unavailable = tuple(sorted(
        item.capability_id for item in failed_items if not _has_version_evidence(item)
    ))
    upstream: set[str] = set()
    for entry in tuple(getattr(component, "uncertainties", ()) or ()):
        if isinstance(entry, str) and entry.startswith("upstream_component_failed:"):
            upstream.update(part for part in entry.split(":", 1)[1].split(",") if part)
    return classify_failure(
        tuple(getattr(component, "checks", ()) or ()),
        missing_capabilities=missing,
        failed_capabilities=mismatched,
        failed_upstream=tuple(sorted(upstream)),
        unavailable_capabilities=unavailable,
    )
