"""Boot Doctor: one-shot per session, non-interactive, failure-isolated."""
from __future__ import annotations

import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import Mock

from realmheart_doctor.boot import run_boot
from realmheart_doctor.health import HealthCheckReport, HealthCheckResult, HealthStatus


def _registry():
    from dataclasses import replace
    from realmheart_maintenance.manifest import load_manifest

    registry = load_manifest(Path("components"))
    component = replace(registry.components["realmheart-core"], realmheart_dependencies=())
    definition = registry.health_checks["check.core.binary.exists"]
    artifact = registry.artifacts[definition.artifact_id]
    return replace(registry, components={component.id: component}, component_order=(component.id,),
                   capabilities={}, artifacts={artifact.id: artifact},
                   health_checks={definition.id: definition})


def _executor(status: HealthStatus):
    executor = Mock()
    executor.execute.return_value = HealthCheckReport(
        (HealthCheckResult("check.core.binary.exists", "realmheart-core", "artifact_exists",
                           status, "observed"),), 0)
    return executor


class DoctorBootTests(unittest.TestCase):
    def test_boot_records_state_once_per_session(self):
        executor = _executor(HealthStatus.PASS)
        with tempfile.TemporaryDirectory() as temp:
            state = Path(temp) / "state"
            first = run_boot(_registry(), state_root=state, session_key="sess-1",
                             executor=executor, notifier=lambda title, body: None,
                             now=datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc))
            second = run_boot(_registry(), state_root=state, session_key="sess-1",
                              executor=executor, notifier=lambda title, body: None,
                              now=datetime(2026, 9, 17, 13, 0, tzinfo=timezone.utc))
            self.assertEqual(first.mode, "ran")
            self.assertEqual(second.mode, "already_ran")
            self.assertEqual(executor.execute.call_count, 1)
            self.assertTrue((state / "current.json").is_file())
            self.assertEqual(len(list((state / "sessions").glob("*.json"))), 1)

    def test_boot_uses_background_context_with_cheap_checks(self):
        executor = _executor(HealthStatus.PASS)
        with tempfile.TemporaryDirectory() as temp:
            run_boot(_registry(), state_root=Path(temp) / "state", session_key="sess-background",
                     executor=executor, notifier=lambda title, body: None)
        _, kwargs = executor.execute.call_args
        self.assertEqual(kwargs.get("context"), "doctor_background")
        self.assertEqual(kwargs.get("max_cost"), "cheap")

    def test_boot_failure_creates_one_incident_and_notifies_once(self):
        executor = _executor(HealthStatus.FAIL)
        calls: list[tuple[str, str]] = []
        with tempfile.TemporaryDirectory() as temp:
            state = Path(temp) / "state"
            outcome = run_boot(_registry(), state_root=state, session_key="s",
                               executor=executor, notifier=lambda t, b: calls.append((t, b)))
            self.assertEqual(outcome.mode, "ran")
            self.assertEqual(len(calls), 1)
            self.assertIn("RH-", calls[0][1])
            self.assertEqual(len(list((state / "incidents").glob("RH-*.json"))), 1)

    def test_session_key_cannot_escape_state_directory(self):
        executor = _executor(HealthStatus.PASS)
        with tempfile.TemporaryDirectory() as temp:
            state = Path(temp) / "state"
            run_boot(_registry(), state_root=state, session_key="../../evil",
                     executor=executor, notifier=lambda title, body: None)
            sessions = list((state / "sessions").glob("*.json"))
            self.assertEqual(len(sessions := sessions), 1)
            self.assertNotIn("..", sessions[0].name)


if __name__ == "__main__":
    unittest.main()
