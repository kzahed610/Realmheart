"""Retention bounds persistent Doctor state without losing live evidence."""
import json
from pathlib import Path
import tempfile
import unittest

from realmheart_doctor.retention import apply_retention


def _incident_payload(component_id: str, resolved: bool, day: str) -> dict:
    return {
        "format_version": 1,
        "id": f"RH-{day}-001",
        "component_id": component_id,
        "health_state": "failed",
        "resolution_state": "resolved" if resolved else "unresolved",
        "timeline": [],
    }


class RetentionTests(unittest.TestCase):
    def test_keeps_current_lkg_and_unresolved_prunes_resolved_beyond_budget(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            incidents = root / "incidents"
            incidents.mkdir(parents=True)
            unresolved = incidents / "RH-20260901-001.json"
            unresolved.write_text(json.dumps(_incident_payload("demo", False, "20260901")))
            resolved_paths = []
            for index in range(1, 25):
                day = f"202608{index:02d}"
                path = incidents / f"RH-{day}-001.json"
                path.write_text(json.dumps(_incident_payload("demo", True, day)))
                resolved_paths.append(path)
            removed = apply_retention(root, resolved_limit=20)
            self.assertEqual(removed["resolved_removed"], 4)
            self.assertTrue(unresolved.is_file())
            surviving = sorted(p.name for p in incidents.glob("RH-*.json"))
            self.assertEqual(len(surviving), 21)
            self.assertIn(unresolved.name, surviving)
            newest = sorted(p.stem for p in resolved_paths)[-4:]
            for name in newest:
                self.assertIn(f"{name}.json", surviving)

    def test_identical_history_snapshots_compact_to_last_seen(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            history = root / "history"
            history.mkdir(parents=True)
            first = history / "snap-20260901T000000.json"
            second = history / "snap-20260902T000000.json"
            snapshot = {"format_version": 1, "overall": "healthy",
                        "components": {"demo": {"status": "healthy", "checks": []}}}
            first.write_text(json.dumps(snapshot))
            second.write_text(json.dumps({**snapshot, "last_seen": "20260902T000000"}))
            summary = apply_retention(root)
            self.assertEqual(summary["compacted"], 1)
            self.assertFalse(first.is_file())
            payload = json.loads(second.read_text())
            self.assertIn("last_seen", payload)

    def test_corrupt_files_are_quarantined_not_crashed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            incidents = root / "incidents"
            incidents.mkdir(parents=True)
            (incidents / "RH-20260901-001.json").write_text("{broken")
            removed = apply_retention(root)
            self.assertIn("quarantined", removed or {})
            quarantined = list((root / "corrupt").iterdir())
            self.assertEqual(len(quarantined), 1)
            self.assertTrue(quarantined[0].read_text().startswith("{broken"))

    def test_reports_directory_is_never_touched(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            reports = root / "reports"
            reports.mkdir(parents=True)
            keep = reports / "RH-20260901-001.md"
            keep.write_text("report")
            apply_retention(root)
            self.assertTrue(keep.is_file())
