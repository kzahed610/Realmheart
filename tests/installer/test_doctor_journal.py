"""Doctor's own log: bounded, local-only, never a secret carrier."""
import json
import os
from pathlib import Path
import tempfile
import unittest

from realmheart_doctor.journal import journal, read_journal


class JournalTests(unittest.TestCase):
    def test_events_append_as_json_lines_with_timestamps(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            journal(root, "state_write", component="demo")
            journal(root, "manifest_load", component=None)
            lines = (root / "doctor.log").read_text().strip().splitlines()
            self.assertEqual(len(lines), 2)
            first, second = (json.loads(line) for line in lines)
            self.assertEqual(first["event"], "state_write")
            self.assertEqual(first["component"], "demo")
            self.assertIn("timestamp", first)
            self.assertEqual(second["event"], "manifest_load")

    def test_oversized_log_rotates_and_keeps_recent_tail(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            log = root / "doctor.log"
            log.write_text('{"event": "old"}\n' * 40)
            for index in range(5):
                journal(root, f"event_{index}", max_bytes=200, keep_tail=10)
            self.assertTrue((root / "doctor.log.1").exists())
            content = (root / "doctor.log").read_text().strip().splitlines()
            self.assertLessEqual(len(content), 11)
            self.assertEqual(json.loads(content[-1])["event"], "event_4")

    def test_failures_never_break_callers_and_never_log_values(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "not-a-dir-yet"
            journal(root, "probe_failure", detail="password=fixture-only")
            self.assertTrue((root / "doctor.log").exists())
            text = (root / "doctor.log").read_text()
            self.assertIn("probe_failure", text)
            self.assertNotIn("fixture-only", text)

    def test_read_journal_returns_recent_events(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            journal(root, "first")
            journal(root, "second")
            events = read_journal(root)
            self.assertEqual([e["event"] for e in events], ["first", "second"])
            self.assertEqual(read_journal(root / "missing"), [])

    def test_state_incidents_and_notifications_write_journal_events(self):
        from realmheart_doctor.diagnosis import ComponentHealth
        from realmheart_doctor.incidents import record_component_failure, record_component_recovery
        from realmheart_doctor.notify import dispatch_notifications
        from realmheart_doctor.state import record_diagnosis
        from .test_doctor_incidents import _diagnosis

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            record_diagnosis(root, _diagnosis(ComponentHealth.FAILED))
            event = record_component_failure(root, "demo")
            dispatch_notifications(root, lambda title, body: None)
            record_diagnosis(root, _diagnosis(ComponentHealth.HEALTHY))
            record_component_recovery(root, "demo")
            events = [entry["event"] for entry in read_journal(root)]
        self.assertIn("state_recorded", events)
        self.assertIn("incident_opened", events)
        self.assertIn("notification", events)
        self.assertIn("incident_resolved", events)
        self.assertIsNotNone(event)
