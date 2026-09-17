"""Evidence comparison must use component LKG, not just previous run."""
from __future__ import annotations

from dataclasses import replace
from datetime import datetime, timezone
import json
from pathlib import Path
import tempfile
import unittest

from .test_doctor_state import _diagnosis
from realmheart_doctor.diagnosis import ComponentHealth
from realmheart_doctor.state import record_diagnosis
from realmheart_doctor.incidents import record_component_failure


class DoctorChangeTests(unittest.TestCase):
    def test_repeated_failure_preserves_changes_since_component_last_healthy(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            good = _diagnosis(ComponentHealth.HEALTHY)
            record_diagnosis(root, good, now=datetime(2026, 9, 17, tzinfo=timezone.utc))
            bad = replace(_diagnosis(ComponentHealth.FAILED), release_version="0.7.9")
            for hour in (1, 2):
                now = datetime(2026, 9, 17, hour, tzinfo=timezone.utc)
                record_diagnosis(root, bad, now=now)
                event = record_component_failure(root, "demo", now=now)
                assert event is not None
                incident = json.loads((root / "incidents" / f"{event.incident_id}.json").read_text())
                changes = incident["relevant_changes"]
                self.assertEqual(len(changes), 1)
                self.assertEqual(changes[0]["type"], "REALMHEART_VERSION_CHANGED")
                self.assertEqual(changes[0]["previous"], "0.7.8")
                self.assertEqual(changes[0]["current"], "0.7.9")
                self.assertFalse(changes[0]["proves_causation"])

    def test_first_failure_has_no_invented_change_history(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            record_diagnosis(root, _diagnosis(ComponentHealth.FAILED))
            event = record_component_failure(root, "demo")
            assert event is not None
            incident = json.loads((root / "incidents" / f"{event.incident_id}.json").read_text())
            self.assertEqual(incident["relevant_changes"], [])
            self.assertIsNone(incident["last_known_good"])
