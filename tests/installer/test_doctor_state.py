"""Doctor temporal state: snapshots, LKG, and durable history contracts."""
from __future__ import annotations

import json
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from realmheart_doctor.diagnosis import ComponentDiagnosis, ComponentHealth, Diagnosis
from realmheart_doctor.health import HealthCheckResult, HealthStatus
from realmheart_doctor.state import record_diagnosis


def _diagnosis(status: ComponentHealth, *, component_id: str = "demo") -> Diagnosis:
    return Diagnosis(
        release_version="0.7.8",
        manifest_digest="a" * 64,
        overall=status,
        components=(ComponentDiagnosis(
            component_id, "Demo", "core", status,
            (HealthCheckResult("check.demo", component_id, "artifact_exists",
                               HealthStatus.PASS if status is ComponentHealth.HEALTHY else HealthStatus.FAIL,
                               "fixture"),),
        ),),
        budget_exhausted=False,
    )


class DoctorStateTests(unittest.TestCase):
    def test_directory_sync_io_failure_is_not_reported_as_success(self):
        import errno
        from unittest.mock import patch
        from realmheart_doctor.state import _atomic_write_json
        with tempfile.TemporaryDirectory() as temp:
            with patch("realmheart_doctor.state.os.fsync", side_effect=[None, OSError(errno.EIO, "sync failed")]):
                with self.assertRaises(OSError):
                    _atomic_write_json(Path(temp) / "state.json", {"format_version": 1})

    def test_healthy_result_updates_last_known_good(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            now = datetime(2026, 9, 17, 12, 0, 0, tzinfo=timezone.utc)
            record = record_diagnosis(root, _diagnosis(ComponentHealth.HEALTHY), now=now)
            current = json.loads((root / "current.json").read_text())
            self.assertEqual(current["format_version"], 1)
            self.assertEqual(current["components"]["demo"]["status"], "healthy")
            lkg = json.loads((root / "components" / "demo" / "last-healthy.json").read_text())
            self.assertEqual(lkg["format_version"], 1)
            self.assertEqual(lkg["status"], "healthy")
            self.assertEqual(lkg["captured_at"], now.isoformat())
            self.assertEqual(record.recovered, ())

    def test_non_healthy_results_never_overwrite_last_known_good(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first = datetime(2026, 9, 17, 12, 0, 0, tzinfo=timezone.utc)
            later = datetime(2026, 9, 17, 13, 0, 0, tzinfo=timezone.utc)
            record_diagnosis(root, _diagnosis(ComponentHealth.HEALTHY), now=first)
            for status in (ComponentHealth.DEGRADED, ComponentHealth.FAILED, ComponentHealth.UNKNOWN):
                record_diagnosis(root, _diagnosis(status), now=later)
                lkg = json.loads((root / "components" / "demo" / "last-healthy.json").read_text())
                self.assertEqual(lkg["status"], "healthy")
                self.assertEqual(lkg["captured_at"], first.isoformat())

    def test_corrupt_current_state_is_isolated_not_fatal(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "current.json").write_text("{not json", encoding="utf-8")
            record = record_diagnosis(
                root, _diagnosis(ComponentHealth.HEALTHY),
                now=datetime(2026, 9, 17, 12, 0, 0, tzinfo=timezone.utc),
            )
            self.assertEqual(record.recovered, ("current.json",))
            self.assertTrue((root / "corrupt").is_dir())
            quarantined = list((root / "corrupt").iterdir())
            self.assertEqual(len(quarantined), 1)
            self.assertEqual(quarantined[0].read_text(), "{not json")
            self.assertTrue(json.loads((root / "current.json").read_text())["components"])

    def test_corrupt_lkg_is_isolated_and_reevaluated(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lkg_dir = root / "components" / "demo"
            lkg_dir.mkdir(parents=True)
            (lkg_dir / "last-healthy.json").write_text("]]]", encoding="utf-8")
            record = record_diagnosis(
                root, _diagnosis(ComponentHealth.HEALTHY),
                now=datetime(2026, 9, 17, 12, 0, 0, tzinfo=timezone.utc),
            )
            self.assertIn("components/demo/last-healthy.json", record.recovered)
            self.assertEqual(
                json.loads((lkg_dir / "last-healthy.json").read_text())["status"],
                "healthy",
            )


if __name__ == "__main__":
    unittest.main()
