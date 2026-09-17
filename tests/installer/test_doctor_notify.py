"""Notifications fire once per unresolved state and never break health logic."""
from datetime import datetime, timezone
from pathlib import Path
import tempfile
import unittest

from .test_doctor_state import _diagnosis
from realmheart_doctor.diagnosis import ComponentHealth
from realmheart_doctor.incidents import record_component_failure
from realmheart_doctor.state import record_diagnosis
from realmheart_doctor.notify import dispatch_notifications


def _failed_incident(root: Path, now: datetime) -> str:
    record_diagnosis(root, _diagnosis(ComponentHealth.FAILED), now=now)
    event = record_component_failure(root, "demo", now=now)
    assert event is not None
    return event.incident_id


class DoctorNotifyTests(unittest.TestCase):
    def test_new_failure_notifies_once_with_incident_id(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            now = datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc)
            incident_id = _failed_incident(root, now)
            calls: list[tuple[str, str]] = []
            dispatch_notifications(root, lambda title, body: calls.append((title, body)), now=now)
            self.assertEqual(len(calls), 1)
            self.assertIn(incident_id, calls[0][1])
            self.assertIn("realmheart doctor --incident", calls[0][1])

    def test_same_unresolved_incident_is_silent_on_repeat(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            now = datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc)
            _failed_incident(root, now)
            dispatch_notifications(root, lambda title, body: None, now=now)
            calls: list[tuple[str, str]] = []
            later = datetime(2026, 9, 17, 13, 0, tzinfo=timezone.utc)
            dispatch_notifications(root, lambda title, body: calls.append((title, body)), now=later)
            self.assertEqual(calls, [])

    def test_no_incidents_is_silent_and_healthy_paths_never_notify(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            record_diagnosis(root, _diagnosis(ComponentHealth.HEALTHY),
                             now=datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc))
            calls: list[tuple[str, str]] = []
            dispatch_notifications(root, lambda title, body: calls.append((title, body)))
            self.assertEqual(calls, [])

    def test_notifier_failure_does_not_raise_and_allows_retry(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            now = datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc)
            _failed_incident(root, now)
            def broken(title: str, body: str) -> None:
                raise RuntimeError("no notification backend")
            results = dispatch_notifications(root, broken, now=now)
            self.assertEqual(results[0]["notified"], False)
            calls: list[tuple[str, str]] = []
            results = dispatch_notifications(root, lambda title, body: calls.append((title, body)),
                                             now=datetime(2026, 9, 17, 13, 0, tzinfo=timezone.utc))
            self.assertEqual(results[0]["notified"], True)


if __name__ == "__main__":
    unittest.main()
