"""Boot Doctor: one-shot per session, non-interactive, failure-isolated."""
from __future__ import annotations

import json
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
                             executor=executor, notifier=lambda title, body, severity=None, **metadata: None,
                             now=datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc))
            second = run_boot(_registry(), state_root=state, session_key="sess-1",
                              executor=executor, notifier=lambda title, body, severity=None, **metadata: None,
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
                     executor=executor, notifier=lambda title, body, severity=None, **metadata: None)
        _, kwargs = executor.execute.call_args
        self.assertEqual(kwargs.get("context"), "doctor_background")
        self.assertEqual(kwargs.get("max_cost"), "cheap")

    def test_boot_consumes_a_pending_package_update_marker(self):
        executor = _executor(HealthStatus.PASS)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = root / "state"
            marker = root / "post-update.pending"
            marker.write_text("")
            log = root / "pacman.log"
            log.write_text("", encoding="utf-8")
            run_boot(_registry(), state_root=state, session_key="sess-update", executor=executor,
                     notifier=lambda title, body, severity=None, **metadata: None, marker_path=marker, log_path=log)
            session = next((state / "sessions").glob("*.json"))
            payload = json.loads(session.read_text(encoding="utf-8"))
            self.assertIsNotNone(payload["package_updates"])
            self.assertTrue(payload["package_updates"]["marker_consumed"])
            self.assertTrue((state / "post-update.json").is_file())

    def test_default_session_key_prefers_the_compositor_then_the_boot_id(self):
        import os
        from unittest.mock import patch

        from realmheart_doctor.boot import default_session_key

        with patch.dict(os.environ, {"HYPRLAND_INSTANCE_SIGNATURE": "sig-1"}, clear=False):
            self.assertEqual(default_session_key(), "sig-1")

        without_signature = {key: value for key, value in os.environ.items()
                             if key != "HYPRLAND_INSTANCE_SIGNATURE"}
        with patch.dict(os.environ, without_signature, clear=True):
            key = default_session_key()
        self.assertIsNotNone(key)
        self.assertTrue(key.startswith("boot-"))

        with patch.dict(os.environ, without_signature, clear=True), \
             patch("realmheart_doctor.boot.Path.read_text", side_effect=OSError("unreadable")):
            self.assertIsNone(default_session_key())

    def test_boot_failure_creates_one_incident_and_notifies_once(self):
        executor = _executor(HealthStatus.FAIL)
        calls: list[tuple[str, str]] = []
        with tempfile.TemporaryDirectory() as temp:
            state = Path(temp) / "state"
            outcome = run_boot(_registry(), state_root=state, session_key="s",
                               executor=executor, notifier=lambda t, b, severity=None, **metadata: calls.append((t, b)))
            self.assertEqual(outcome.mode, "ran")
            self.assertEqual(len(calls), 1)
            self.assertIn("RH-", calls[0][1])
            self.assertEqual(len(list((state / "incidents").glob("RH-*.json"))), 1)

    def test_boot_resolves_event_surface_incident_after_verified_recovery(self):
        failed = _executor(HealthStatus.FAIL)
        healthy = _executor(HealthStatus.PASS)
        resolved: list[str] = []
        with tempfile.TemporaryDirectory() as temp:
            state = Path(temp) / "state"
            run_boot(
                _registry(), state_root=state, session_key="failed-session",
                executor=failed,
                notifier=lambda title, body, severity=None, **metadata: None,
            )
            incident = next((state / "incidents").glob("RH-*.json")).stem
            run_boot(
                _registry(), state_root=state, session_key="healthy-session",
                executor=healthy,
                notifier=lambda title, body, severity=None, **metadata: None,
                resolver=resolved.append,
            )
        self.assertEqual(resolved, [incident])

    def test_boot_resolves_all_open_event_surface_incidents_for_component(self):
        failed = _executor(HealthStatus.FAIL)
        healthy = _executor(HealthStatus.PASS)
        resolved: list[str] = []
        with tempfile.TemporaryDirectory() as temp:
            state = Path(temp) / "state"
            run_boot(
                _registry(), state_root=state, session_key="failed-session",
                executor=failed,
                notifier=lambda title, body, severity=None, **metadata: None,
            )
            first = next((state / "incidents").glob("RH-*.json"))
            second = first.with_name("RH-20260920-999.json")
            payload = json.loads(first.read_text(encoding="utf-8"))
            payload["id"] = second.stem
            payload["failure_fingerprint"] = "alternate-fingerprint"
            second.write_text(json.dumps(payload), encoding="utf-8")

            run_boot(
                _registry(), state_root=state, session_key="healthy-session",
                executor=healthy,
                notifier=lambda title, body, severity=None, **metadata: None,
                resolver=resolved.append,
            )

        self.assertEqual(set(resolved), {first.stem, second.stem})

    def test_session_key_cannot_escape_state_directory(self):
        executor = _executor(HealthStatus.PASS)
        with tempfile.TemporaryDirectory() as temp:
            state = Path(temp) / "state"
            run_boot(_registry(), state_root=state, session_key="../../evil",
                     executor=executor, notifier=lambda title, body, severity=None, **metadata: None)
            sessions = list((state / "sessions").glob("*.json"))
            self.assertEqual(len(sessions := sessions), 1)
            self.assertNotIn("..", sessions[0].name)


if __name__ == "__main__":
    unittest.main()
