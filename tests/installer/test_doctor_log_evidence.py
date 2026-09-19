"""Component-log evidence: bounded, sanitized, captured once per incident."""
from __future__ import annotations

import json
import os
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch

from realmheart_maintenance.manifest import load_manifest
from realmheart_doctor.diagnosis import ComponentHealth
from realmheart_doctor.incidents import record_component_failure
from realmheart_doctor.log_evidence import (
    MAX_LINES,
    collect_log_evidence,
    log_collector_for,
)
from realmheart_doctor.state import record_diagnosis
from .test_doctor_incidents import _diagnosis

_FILE_MANIFEST = '''schema_version = 1
release_version = "0.7.8"

[[components]]
id = "demo"
name = "Demo"
component_version = "release"
category = "core"
stage = "foundation"
[[components.log_sources]]
kind = "file"
target = "$XDG_STATE_HOME/realmheart/doctor/demo.log"
'''


class _Result:
    def __init__(self, returncode: int, stdout: str = "") -> None:
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = ""


class LogEvidenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.registry = load_manifest(Path("components"))

    def test_journal_evidence_uses_structured_argv_and_is_sanitized(self):
        calls: list[tuple[str, ...]] = []

        def runner(argv, timeout):
            calls.append(tuple(argv))
            body = "\n".join(f"log line {index}" for index in range(100))
            return _Result(0, "token=ghp_AAAABBBBCCCCDDDDEEEEFFFF000011112222\n" + body)

        evidence = collect_log_evidence(self.registry, "event-surface", runner=runner,
                                        now=datetime(2026, 9, 18, tzinfo=timezone.utc))
        self.assertIsNotNone(evidence)
        self.assertEqual(evidence["collected_at"], "2026-09-18T00:00:00+00:00")
        source = evidence["sources"][0]
        self.assertEqual((source["kind"], source["target"]), ("journal", "realmheart-eventd.service"))
        self.assertTrue(source["sanitized"])
        self.assertEqual(len(source["lines"]), MAX_LINES)
        self.assertTrue(all("ghp_" not in line for line in source["lines"]))
        self.assertEqual(calls[0][:3], (calls[0][0], "--user", "-u"))
        self.assertIn("realmheart-eventd.service", calls[0])
        self.assertIn("--no-pager", calls[0])

    def test_journal_failure_or_undeclared_component_yields_no_evidence(self):
        self.assertIsNone(collect_log_evidence(
            self.registry, "event-surface", runner=lambda argv, timeout: _Result(1, "no logs")))
        self.assertIsNone(collect_log_evidence(
            self.registry, "screenshot", runner=lambda argv, timeout: _Result(0, "unexpected")))
        self.assertIsNone(collect_log_evidence(self.registry, "not-a-component"))

    def test_file_evidence_is_tail_bounded_and_symlinks_are_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry_dir = root / "components"
            registry_dir.mkdir()
            (registry_dir / "demo.toml").write_text(_FILE_MANIFEST, encoding="utf-8")
            registry = load_manifest(registry_dir)
            log_dir = root / "state" / "realmheart" / "doctor"
            log_dir.mkdir(parents=True)
            log = log_dir / "demo.log"
            log.write_text("\n".join(f"line {index}" for index in range(100)) + "\n", encoding="utf-8")
            with patch.dict(os.environ, {"XDG_STATE_HOME": str(root / "state")}):
                evidence = collect_log_evidence(registry, "demo")
                self.assertIsNotNone(evidence)
                lines = evidence["sources"][0]["lines"]
                self.assertEqual(len(lines), MAX_LINES)
                self.assertEqual(lines[-1], "line 99")

                (log_dir / "target.log").write_text("secret\n", encoding="utf-8")
                log.unlink()
                log.symlink_to(log_dir / "target.log")
                self.assertIsNone(collect_log_evidence(registry, "demo"))

    def test_collector_never_raises_and_never_breaks_incident_creation(self):
        def bomb(argv, timeout):
            raise OSError("journalctl exploded")

        collector = log_collector_for(self.registry, runner=bomb)
        self.assertIsNone(collector("event-surface"))

        def failing(_component_id):
            raise RuntimeError("collector exploded")

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            record_diagnosis(root, _diagnosis(ComponentHealth.FAILED))
            event = record_component_failure(root, "demo", log_collector=failing)
            self.assertIsNotNone(event)
            payload = json.loads((root / "incidents" / f"{event.incident_id}.json").read_text(encoding="utf-8"))
        self.assertIsNone(payload["raw_logs"])

    def test_logs_are_collected_once_when_the_incident_is_created(self):
        calls: list[str] = []

        def collector(component_id):
            calls.append(component_id)
            return {"sources": [{"kind": "journal", "target": "unit.service",
                                 "lines": ["failure observed"], "sanitized": True}]}

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            record_diagnosis(root, _diagnosis(ComponentHealth.FAILED))
            first = record_component_failure(root, "demo", log_collector=collector)
            payload = json.loads((root / "incidents" / f"{first.incident_id}.json").read_text(encoding="utf-8"))
            record_component_failure(root, "demo", log_collector=collector)
        self.assertEqual(payload["raw_logs"]["sources"][0]["lines"], ["failure observed"])
        self.assertEqual(calls, ["demo"])


if __name__ == "__main__":
    unittest.main()
