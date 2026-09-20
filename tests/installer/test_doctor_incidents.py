"""Doctor incidents: failure classification and deduplicated incident files."""
from __future__ import annotations

import json
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from realmheart_doctor.diagnosis import ComponentDiagnosis, ComponentHealth, Diagnosis
from realmheart_doctor.health import HealthCheckResult, HealthStatus
from realmheart_doctor.state import record_diagnosis
from realmheart_doctor.incidents import record_component_failure, record_component_recovery


def _diagnosis(status: ComponentHealth, *, component_id: str = "demo") -> Diagnosis:
    return Diagnosis(
        release_version="0.7.8",
        manifest_digest="a" * 64,
        overall=status,
        components=(ComponentDiagnosis(
            component_id, "Demo", "core", status,
            (HealthCheckResult("check.demo", component_id, "artifact_exists",
                               HealthStatus.PASS if status is ComponentHealth.HEALTHY else HealthStatus.FAIL,
                               "artifact_present" if status is ComponentHealth.HEALTHY else "artifact_missing"),),
        ),),
        budget_exhausted=False,
    )


class DoctorIncidentTests(unittest.TestCase):
    def test_upstream_only_failure_records_the_named_dependency(self):
        from realmheart_doctor.diagnosis import ComponentDiagnosis, Diagnosis
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            diagnosis = Diagnosis(
                release_version="0.7.8", manifest_digest="a" * 64, overall=ComponentHealth.FAILED,
                components=(ComponentDiagnosis(
                    "session", "Session", "essential", ComponentHealth.FAILED, (),
                    ("upstream_component_failed:realmheart-core",),
                ),),
                budget_exhausted=False,
            )
            record_diagnosis(root, diagnosis)
            event = record_component_failure(root, "session")
            payload = json.loads((root / "incidents" / f"{event.incident_id}.json").read_text(encoding="utf-8"))
        self.assertEqual(payload["failure_class"], "COMPONENT_DEPENDENCY_FAILURE")
        self.assertEqual(payload["checks"], [])
        self.assertEqual(payload["symptoms"], ["required dependency failed: realmheart-core"])
        self.assertIn("realmheart-core", payload["observed"])

    def test_recovery_requires_current_healthy_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            record_diagnosis(root, _diagnosis(ComponentHealth.FAILED))
            event = record_component_failure(root, "demo")
            self.assertIsNone(record_component_recovery(root, "demo"))
            incident = json.loads((root / "incidents" / f"{event.incident_id}.json").read_text())
            self.assertEqual(incident["resolution_state"], "unresolved")

    def test_distinct_failure_condition_creates_distinct_incident(self):
        from dataclasses import replace
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            diagnosis = _diagnosis(ComponentHealth.FAILED)
            record_diagnosis(root, diagnosis)
            first = record_component_failure(root, "demo")
            component = diagnosis.components[0]
            check = replace(component.checks[0], reason_code="permission_denied")
            diagnosis = replace(diagnosis, components=(replace(component, checks=(check,)),))
            record_diagnosis(root, diagnosis)
            second = record_component_failure(root, "demo")
            self.assertNotEqual(first.incident_id, second.incident_id)

    def test_first_failure_creates_incident_with_stable_id_and_timeline(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            now = datetime(2026, 9, 17, 12, 0, 0, tzinfo=timezone.utc)
            record_diagnosis(root, _diagnosis(ComponentHealth.FAILED), now=now)
            event = record_component_failure(root, "demo", now=now)
            incident = json.loads((root / "incidents" / f"{event.incident_id}.json").read_text())
            self.assertRegex(event.incident_id, r"^RH-20260917-\d{3}$")
            self.assertEqual(incident["format_version"], 1)
            self.assertEqual(incident["component_id"], "demo")
            self.assertEqual(incident["failure_class"], "COMPONENT_ARTIFACT_MISSING")
            self.assertEqual(incident["confidence"], "HIGH")
            self.assertEqual(incident["resolution_state"], "unresolved")
            self.assertEqual(len(incident["timeline"]), 1)
            self.assertEqual(incident["timeline"][0]["event_type"], "HEALTH_CHECK_FAILED")

    def test_incident_failure_class_follows_observed_evidence(self):
        from realmheart_doctor.diagnosis import ComponentDiagnosis, Diagnosis
        from realmheart_doctor.health import HealthCheckResult, HealthStatus
        check = HealthCheckResult("check.version", "demo", "version_probe",
                                  HealthStatus.FAIL, "version_mismatch")
        diagnosis = Diagnosis(
            release_version="0.7.8",
            manifest_digest="a" * 64,
            overall=ComponentHealth.FAILED,
            components=(ComponentDiagnosis("demo", "Demo", "core",
                                           ComponentHealth.FAILED, (check,)),),
            budget_exhausted=False,
        )
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            record_diagnosis(root, diagnosis)
            event = record_component_failure(root, "demo")
            assert event is not None
            incident = json.loads((root / "incidents" / f"{event.incident_id}.json").read_text())
            self.assertEqual(incident["failure_class"], "DEPENDENCY_VERSION_MISMATCH")
            self.assertEqual(incident["confidence"], "HIGH")
            self.assertEqual(incident["checks"][0]["check_id"], "check.version")

    def test_repeat_failure_appends_observation_instead_of_new_incident(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first = datetime(2026, 9, 17, 12, 0, 0, tzinfo=timezone.utc)
            later = datetime(2026, 9, 17, 13, 0, 0, tzinfo=timezone.utc)
            record_diagnosis(root, _diagnosis(ComponentHealth.FAILED), now=first)
            first_event = record_component_failure(root, "demo", now=first)
            second_event = record_component_failure(root, "demo", now=later)
            self.assertEqual(first_event.incident_id, second_event.incident_id)
            incident = json.loads((root / "incidents" / f"{second_event.incident_id}.json").read_text())
            self.assertEqual(len(incident["timeline"]), 2)
            self.assertEqual(incident["timeline"][1]["event_type"], "OBSERVATION_APPENDED")
            self.assertEqual(len(list((root / "incidents").glob("*.json"))), 1)

    def test_recovery_marks_incident_resolved_and_next_failure_starts_fresh(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first = datetime(2026, 9, 17, 12, 0, 0, tzinfo=timezone.utc)
            healthy = datetime(2026, 9, 17, 12, 30, 0, tzinfo=timezone.utc)
            relapse = datetime(2026, 9, 17, 13, 0, 0, tzinfo=timezone.utc)
            record_diagnosis(root, _diagnosis(ComponentHealth.FAILED), now=first)
            record_component_failure(root, "demo", now=first)
            record_diagnosis(root, _diagnosis(ComponentHealth.HEALTHY), now=healthy)
            self.assertIsNone(record_component_failure(root, "demo", now=healthy))
            record_diagnosis(root, _diagnosis(ComponentHealth.FAILED), now=relapse)
            resolved = record_component_failure(root, "demo", now=relapse)
            self.assertNotEqual(resolved.incident_id, "RH-20260917-001")
            incident = json.loads((root / "incidents" / "RH-20260917-001.json").read_text())
            self.assertEqual(incident["resolution_state"], "resolved")
            self.assertEqual(incident["timeline"][-1]["event_type"], "INCIDENT_RESOLVED")

    def test_degraded_component_does_not_create_incident_spam(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            record_diagnosis(root, _diagnosis(ComponentHealth.DEGRADED),
                             now=datetime(2026, 9, 17, 12, 0, 0, tzinfo=timezone.utc))
            event = record_component_failure(root, "demo",
                                             now=datetime(2026, 9, 17, 12, 0, 0, tzinfo=timezone.utc))
            self.assertIsNone(event)
            self.assertFalse((root / "incidents").exists())


if __name__ == "__main__":
    unittest.main()
