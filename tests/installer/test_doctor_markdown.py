"""Saved incidents render locally without new probes or external sharing."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from realmheart_doctor.cli import main


class IncidentReportTests(unittest.TestCase):
    def test_markdown_golden_preserves_unknown_without_empty_sections(self):
        from realmheart_doctor.incident_reports import render_incident

        report = render_incident({"id": "RH-20260917-001", "component_id": "demo",
                                  "health_state": "unknown", "failure_class": "UNKNOWN"})
        self.assertEqual(report["text"],
                         "# Realmheart Doctor incident\n\n## What broke\n\n    demo\n\n"
                         "## Current state\n\n    unknown\n\n## Doctor diagnosis\n\n"
                         "    UNKNOWN\n\n## Incident ID\n\n    RH-20260917-001\n")

    def test_invalid_saved_incidents_fail_cleanly_without_writes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "incidents").mkdir()
            path = root / "incidents" / "RH-20260917-001.json"
            for data in ("{broken", "[]", '{"format_version": 2}', '{"id": "other"}'):
                path.write_text(data)
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    code = main(["--incident", path.stem, "--report", "--state-dir", temp, "--json"])
                self.assertEqual(code, 5)
                self.assertEqual(json.loads(output.getvalue())["error"], "incident_report_failed")
                self.assertFalse((root / "reports").exists())
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(main(["--incident", "../escape", "--report", "--state-dir", temp]), 5)

    def test_saved_incident_preview_is_read_only(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "incidents").mkdir()
            path = root / "incidents" / "RH-20260917-001.json"
            path.write_text(json.dumps({"format_version": 1, "id": path.stem, "observed": "missing"}))
            with contextlib.redirect_stdout(io.StringIO()) as output:
                code = main(["--incident", path.stem, "--state-dir", temp])
            self.assertEqual(code, 0)
            self.assertIn("Observed failure", output.getvalue())
            self.assertFalse((root / "reports").exists())

    def test_diagnosis_report_option_exports_current_incident(self):
        from .test_doctor_state import _diagnosis
        from realmheart_doctor.diagnosis import ComponentHealth

        with tempfile.TemporaryDirectory() as temp:
            with patch("realmheart_doctor.diagnosis.diagnose", return_value=_diagnosis(ComponentHealth.FAILED)), contextlib.redirect_stdout(io.StringIO()) as output:
                code = main(["doctor", "--state-dir", temp, "--report", "--json"])
            self.assertEqual(code, 2)
            paths = json.loads(output.getvalue())["reports"]
            self.assertEqual(len(paths), 1)
            self.assertTrue(Path(paths[0]).is_file())
            self.assertIn("demo", Path(paths[0]).read_text())

    def test_report_option_without_state_is_invalid(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(main(["doctor", "--report", "--json"]), 4)


    def test_saved_incident_cli_writes_sanitized_markdown_without_diagnosis(self):
        incident = {"format_version": 1, "id": "RH-20260917-001",
                    "component_id": "demo", "health_state": "failed",
                    "failure_class": "UNKNOWN", "confidence": "LOW",
                    "observed": "backend 192.168.1.15 unavailable",
                    "repair_attempts": [{"result": "verification_failed"}],
                    "resolution_state": "unresolved", "timeline": []}
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "incidents").mkdir()
            (root / "incidents" / (incident["id"] + ".json")).write_text(json.dumps(incident))
            output = io.StringIO()
            with patch("realmheart_doctor.diagnosis.diagnose", side_effect=AssertionError("no new probes")), contextlib.redirect_stdout(output):
                code = main(["--incident", incident["id"], "--report", "--state-dir", temp, "--json"])
            self.assertEqual(code, 0)
            payload = json.loads(output.getvalue())
            report = Path(payload["report_path"])
            self.assertEqual(report.parent, root / "reports")
            text = report.read_text()
            self.assertIn("## Repair attempts", text)
            self.assertIn("verification_failed", text)
            self.assertIn("UNKNOWN", text)
            self.assertNotIn("192.168.1.15", text)
            self.assertEqual(report.stat().st_mode & 0o777, 0o600)
