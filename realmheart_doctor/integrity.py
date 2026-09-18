"""Read-only installation integrity view over the shared forensic contract.

This is a focused projection, not a second checker: receipt artifact drift
comes from ``analyze_forensics`` and version evidence comes from the same
bounded read-only health executor the manual Doctor uses.  It never mutates
state and never requires privileges.
"""
from __future__ import annotations

from dataclasses import dataclass

from realmheart_maintenance.forensics import analyze_forensics
from realmheart_maintenance.manifest import ManifestRegistry
from realmheart_maintenance.version import RELEASE_VERSION

from .health import HealthCheckExecutor, HealthStatus

_ERROR_SEVERITIES = frozenset({"critical", "error"})
_VERSION_SEVERITIES = {
    HealthStatus.FAIL: ("INTEGRITY_VERSION_MISMATCH", "error"),
    HealthStatus.WARNING: ("INTEGRITY_VERSION_UNTESTED", "warning"),
    HealthStatus.UNKNOWN: ("INTEGRITY_VERSION_UNKNOWN", "unknown"),
}


@dataclass(frozen=True)
class IntegrityFinding:
    code: str
    severity: str
    subject: str
    summary: str
    expected: str | None = None
    observed: str | None = None

    def to_dict(self) -> dict[str, object]:
        return {
            "code": self.code,
            "severity": self.severity,
            "subject": self.subject,
            "summary": self.summary,
            "expected": self.expected,
            "observed": self.observed,
        }


@dataclass(frozen=True)
class IntegrityReport:
    status: str  # clean | attention | drift
    realmheart_version: str
    manifest_digest: str
    receipt_digest_matches: bool
    findings: tuple[IntegrityFinding, ...]

    def to_dict(self) -> dict[str, object]:
        return {
            "format_version": 1,
            "doctor_version": RELEASE_VERSION,
            "status": self.status,
            "realmheart_version": self.realmheart_version,
            "manifest_digest": self.manifest_digest,
            "receipt_digest_matches": self.receipt_digest_matches,
            "findings": [item.to_dict() for item in self.findings],
        }


def _version_findings(registry: ManifestRegistry, snapshot, executor) -> list[IntegrityFinding]:
    check_ids = tuple(
        check_id
        for check_id, spec in registry.health_checks.items()
        if spec.check == "version_probe"
        and spec.artifact_id is not None
        and (observation := snapshot.artifacts.get(spec.artifact_id)) is not None
        and observation.exists
    )
    if not check_ids:
        return []
    run = (executor or HealthCheckExecutor()).execute(
        registry, context="doctor_manual", max_cost="cheap", check_ids=check_ids,
    )
    findings: list[IntegrityFinding] = []
    for result in run.results:
        mapped = _VERSION_SEVERITIES.get(result.status)
        if mapped is None:
            continue
        code, severity = mapped
        definition = registry.health_checks[result.check_id]
        expected = (
            definition.args.get("expected_version")
            or definition.args.get("minimum_version")
            or None
        )
        findings.append(IntegrityFinding(
            code=code,
            severity=severity,
            subject=result.check_id,
            summary=f"{result.check_id} reported {result.reason_code}",
            expected=str(expected) if expected is not None else None,
            observed=result.value,
        ))
    return findings


def assess_integrity(
    registry: ManifestRegistry,
    receipt,
    *,
    executor=None,
) -> IntegrityReport:
    """Return the current integrity view for one accepted installation receipt."""

    from .acceptance import _current_snapshot

    snapshot = _current_snapshot(registry, receipt)
    forensic = analyze_forensics(
        registry, receipt, snapshot, health_context="doctor_manual", max_health_cost="cheap",
    )
    findings: list[IntegrityFinding] = [
        IntegrityFinding(
            code=drift.error_code,
            severity=drift.severity,
            subject=drift.subject_id,
            summary=drift.summary,
            expected=drift.previous,
            observed=drift.current,
        )
        for drift in forensic.drifts
    ]
    findings.extend(_version_findings(registry, snapshot, executor))
    findings.sort(key=lambda item: (item.code, item.subject, item.severity))
    status = "drift" if any(item.severity in _ERROR_SEVERITIES for item in findings) else (
        "attention" if findings else "clean"
    )
    return IntegrityReport(
        status=status,
        realmheart_version=receipt.realmheart_version,
        manifest_digest=registry.digest,
        receipt_digest_matches=receipt.manifest_digest == registry.digest,
        findings=tuple(findings),
    )


def render_integrity(report: IntegrityReport) -> str:
    lines = [f"Realmheart Doctor — integrity {report.status.upper()}"]
    if not report.receipt_digest_matches:
        lines.append("  manifest identity drift: receipt was accepted against a different manifest set")
    for item in report.findings:
        lines.append(f"  {item.severity.upper():8} {item.code} {item.subject}")
        if item.expected is not None or item.observed is not None:
            lines.append(f"    expected={item.expected!r} observed={item.observed!r}")
    return "\n".join(lines)
