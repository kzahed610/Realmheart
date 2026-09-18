"""Golden Markdown: full-incident report rendering is byte-stable."""
from __future__ import annotations

import unittest
from pathlib import Path

from . import _bootstrap
from realmheart_doctor.incident_reports import render_incident

GOLDEN = _bootstrap.REPO_ROOT / "tests" / "installer" / "golden"

_UNRESOLVED = {
    "format_version": 1,
    "id": "RH-20260918-001",
    "component_id": "screenshot",
    "created_at": "2026-09-18T10:00:00+00:00",
    "updated_at": "2026-09-18T10:05:00+00:00",
    "health_state": "failed",
    "failure_class": "COMPONENT_ARTIFACT_MISSING",
    "confidence": "HIGH",
    "symptoms": ["check.screenshot.binary.exists: artifact_missing"],
    "expected": "component passes its canonical health checks",
    "observed": "canonical artifact is absent",
    "last_known_good": {
        "captured_at": "2026-09-17T21:00:00+00:00",
        "release_version": "0.7.8",
        "manifest_digest": "a" * 64,
    },
    "relevant_changes": [
        {"type": "PACKAGE_TRANSACTION", "package": "gtk4", "previous": "4.20.1-1", "current": "4.22.4-1"},
    ],
    "checks": [
        {"check_id": "check.screenshot.binary.exists", "status": "fail", "reason_code": "artifact_missing"},
    ],
    "repair_attempts": [
        {
            "action_type": "REBUILD_COMPONENT",
            "risk": "CONFIRM",
            "fingerprint": "0123456789abcdef",
            "status": "failed",
            "detail": "build failed with status 1",
            "timestamp": "2026-09-18T10:05:00+00:00",
        },
    ],
    "resolution_state": "unresolved",
    "timeline": [
        {
            "timestamp": "2026-09-18T10:00:00+00:00",
            "event_type": "HEALTH_CHECK_FAILED",
            "summary": "component screenshot was diagnosed as failed",
            "details": {"check_id": "check.screenshot.binary.exists"},
        },
        {
            "timestamp": "2026-09-18T10:05:00+00:00",
            "event_type": "REPAIR_ATTEMPTED",
            "summary": "1 repair step(s) recorded; outcome failed",
            "details": {"statuses": ["failed"]},
        },
    ],
}

_RESOLVED = {
    **_UNRESOLVED,
    "updated_at": "2026-09-18T11:00:00+00:00",
    "health_state": "healthy",
    "resolution_state": "resolved",
    "repair_attempts": [
        *_UNRESOLVED["repair_attempts"],
        {
            "action_type": "REBUILD_COMPONENT",
            "risk": "CONFIRM",
            "fingerprint": "fedcba9876543210",
            "status": "succeeded",
            "detail": "rebuilt and reinstalled: realmheart_screenshot",
            "timestamp": "2026-09-18T10:55:00+00:00",
        },
    ],
    "repair_results": [{"action_type": "REBUILD_COMPONENT", "verified": True}],
    "timeline": [
        *_UNRESOLVED["timeline"],
        {
            "timestamp": "2026-09-18T11:00:00+00:00",
            "event_type": "INCIDENT_RESOLVED",
            "summary": "component returned to a reliably verified healthy state",
            "details": {},
        },
    ],
}

_REDACTED = {
    "format_version": 1,
    "id": "RH-20260918-002",
    "component_id": "lockscreen-auth",
    "created_at": "2026-09-18T12:00:00+00:00",
    "updated_at": "2026-09-18T12:00:00+00:00",
    "health_state": "failed",
    "failure_class": "OBSERVED_FAILURE",
    "confidence": "MEDIUM",
    "observed": (
        "helper failed for uid 1000 at /home/somebody/.local/bin/realmheart-auth-helper; "
        "network peer 10.0.0.17; token=ghp_AAAABBBBCCCCDDDDEEEEFFFF000011112222"
    ),
    "resolution_state": "unresolved",
    "timeline": [
        {
            "timestamp": "2026-09-18T12:00:00+00:00",
            "event_type": "HEALTH_CHECK_FAILED",
            "summary": "component lockscreen-auth was diagnosed as failed",
            "details": {"check_id": "check.auth.helper.exists"},
        },
    ],
}


class MarkdownGoldenTests(unittest.TestCase):
    def _assert_golden(self, name: str, incident: dict) -> None:
        expected = (GOLDEN / f"{name}.md").read_text(encoding="utf-8")
        report = render_incident(incident)
        self.assertTrue(report["export_allowed"], report["warnings"])
        self.assertEqual(report["text"], expected, f"golden drift in {name}.md")

    def test_unresolved_incident_matches_golden(self) -> None:
        self._assert_golden("incident_unresolved", _UNRESOLVED)

    def test_resolved_incident_matches_golden(self) -> None:
        self._assert_golden("incident_resolved", _RESOLVED)

    def test_redacted_incident_matches_golden(self) -> None:
        self._assert_golden("incident_redacted", _REDACTED)
        text = render_incident(_REDACTED)["text"]
        self.assertIn("~/.local", text)
        self.assertIn("[REDACTED]", text)
        self.assertNotIn("ghp_", text)
        self.assertNotIn("10.0.0.17", text)


if __name__ == "__main__":
    unittest.main()
