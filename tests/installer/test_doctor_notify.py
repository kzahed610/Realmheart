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
            dispatch_notifications(root, lambda title, body, severity=None: calls.append((title, body)), now=now)
            self.assertEqual(len(calls), 1)
            self.assertIn(incident_id, calls[0][1])
            self.assertIn("realmheart-doctor incident", calls[0][1])

    def test_same_unresolved_incident_is_silent_on_repeat(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            now = datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc)
            _failed_incident(root, now)
            dispatch_notifications(root, lambda title, body, severity=None: None, now=now)
            calls: list[tuple[str, str]] = []
            later = datetime(2026, 9, 17, 13, 0, tzinfo=timezone.utc)
            dispatch_notifications(root, lambda title, body, severity=None: calls.append((title, body)), now=later)
            self.assertEqual(calls, [])

    def test_no_incidents_is_silent_and_healthy_paths_never_notify(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            record_diagnosis(root, _diagnosis(ComponentHealth.HEALTHY),
                             now=datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc))
            calls: list[tuple[str, str]] = []
            dispatch_notifications(root, lambda title, body, severity=None: calls.append((title, body)))
            self.assertEqual(calls, [])

    def test_notifier_failure_does_not_raise_and_allows_retry(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            now = datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc)
            _failed_incident(root, now)
            def broken(title: str, body: str, severity: str = "warning") -> None:
                raise RuntimeError("no notification backend")
            results = dispatch_notifications(root, broken, now=now)
            self.assertEqual(results[0]["notified"], False)
            calls: list[tuple[str, str]] = []
            results = dispatch_notifications(root, lambda title, body, severity=None: calls.append((title, body)),
                                             now=datetime(2026, 9, 17, 13, 0, tzinfo=timezone.utc))
            self.assertEqual(results[0]["notified"], True)


    def test_explicit_false_backend_result_remains_retry_eligible(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            now = datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc)
            _failed_incident(root, now)
            first = dispatch_notifications(root, lambda *_args: False, now=now)
            self.assertFalse(first[0]["notified"])
            calls = []
            second = dispatch_notifications(
                root, lambda *args: calls.append(args),
                now=datetime(2026, 9, 17, 13, 0, tzinfo=timezone.utc),
            )
            self.assertTrue(second[0]["notified"])
            self.assertEqual(len(calls), 1)

    def test_failed_incidents_are_notified_as_critical(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            now = datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc)
            _failed_incident(root, now)
            severities: list[str] = []
            dispatch_notifications(root, lambda title, body, severity="warning": severities.append(severity), now=now)
        self.assertEqual(severities, ["critical"])

    def test_repair_records_are_notified_as_warnings(self):
        from realmheart_doctor.incidents import record_repair_attempt

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            now = datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc)
            record_repair_attempt(root, "demo", ({"action_type": "REBUILD_COMPONENT", "status": "failed"},),
                                  outcome="failed", now=now)
            severities: list[str] = []
            dispatch_notifications(root, lambda title, body, severity="warning": severities.append(severity), now=now)
        self.assertEqual(severities, ["warning"])


class NotifyBackendTests(unittest.TestCase):
    def test_event_surface_backend_uses_structured_argv(self):
        from unittest.mock import patch

        from realmheart_doctor import notify_backends

        calls: list[tuple[str, ...]] = []
        with patch.dict("os.environ", {"REALMHEART_DOCTOR_NOTIFY_BACKEND": "event"}, clear=False), \
             patch.object(notify_backends.shutil, "which", return_value="/usr/bin/realmheart-event"), \
             patch.object(notify_backends.subprocess, "run",
                          side_effect=lambda argv, **kwargs: calls.append(tuple(argv))):
            delivered = notify_backends.deliver("Realmheart regression", "incident RH-1", severity="critical")
        self.assertTrue(delivered)
        argv = calls[0]
        self.assertEqual(argv[0], "/usr/bin/realmheart-event")
        self.assertEqual(argv[1], "send")
        self.assertIn("--source", argv)
        self.assertIn("realmheart-doctor", argv)
        self.assertIn("critical", argv)
        self.assertIn("attention", argv)
        self.assertTrue(argv[argv.index("--id") + 1].startswith("realmheart-doctor-"))

    def test_event_surface_notification_adds_copyable_inspect_action(self):
        from unittest.mock import patch

        from realmheart_doctor import notify_backends

        calls: list[tuple[str, ...]] = []
        body = "Component demo is unresolved.\nInspect: realmheart-doctor incident RH-20260920-001"
        with patch.dict("os.environ", {"REALMHEART_DOCTOR_NOTIFY_BACKEND": "event"}, clear=False), \
             patch.object(notify_backends.shutil, "which", return_value="/usr/bin/realmheart-event"), \
             patch.object(notify_backends.subprocess, "run",
                          side_effect=lambda argv, **kwargs: calls.append(tuple(argv))):
            self.assertTrue(notify_backends.deliver("Realmheart regression", body, severity="critical"))
        argv = calls[0]
        self.assertIn("--action-copy", argv)
        action = argv[argv.index("--action-copy") + 1]
        self.assertEqual(
            action,
            "inspect|Copy Doctor command|realmheart-doctor incident RH-20260920-001",
        )

    def test_event_ids_are_stable_for_the_same_incident(self):
        from unittest.mock import patch

        from realmheart_doctor import notify_backends

        calls: list[tuple[str, ...]] = []
        with patch.dict("os.environ", {"REALMHEART_DOCTOR_NOTIFY_BACKEND": "event"}, clear=False), \
             patch.object(notify_backends.shutil, "which", return_value="/usr/bin/realmheart-event"), \
             patch.object(notify_backends.subprocess, "run",
                          side_effect=lambda argv, **kwargs: calls.append(tuple(argv))):
            notify_backends.deliver("title", "same body")
            notify_backends.deliver("title", "same body")
            notify_backends.deliver("title", "different body")
        ids = [argv[argv.index("--id") + 1] for argv in calls]
        self.assertEqual(ids[0], ids[1])
        self.assertNotEqual(ids[0], ids[2])

    def test_auto_prefers_the_event_surface_and_degrades_to_desktop(self):
        from unittest.mock import patch

        from realmheart_doctor import notify_backends

        with patch.dict("os.environ", {"WAYLAND_DISPLAY": "wayland-1"}, clear=False), \
             patch.object(notify_backends.shutil, "which",
                          side_effect=lambda name: "/usr/bin/realmheart-event" if name == "realmheart-event" else None), \
             patch.object(notify_backends, "_event_surface") as event, \
             patch.object(notify_backends, "_notify_send") as desktop:
            notify_backends.deliver("t", "b")
        event.assert_called_once()
        desktop.assert_not_called()

        with patch.object(notify_backends.shutil, "which", return_value=None), \
             patch.object(notify_backends, "_event_surface") as event, \
             patch.object(notify_backends, "_notify_send") as desktop, \
             patch.dict("os.environ", {"WAYLAND_DISPLAY": "wayland-1"}, clear=False):
            notify_backends.deliver("t", "b")
        desktop.assert_called_once()
        event.assert_not_called()

    def test_kill_switch_and_no_backend_are_quiet(self):
        from unittest.mock import patch

        from realmheart_doctor import notify_backends

        with patch.dict("os.environ", {"REALMHEART_DOCTOR_NOTIFY_BACKEND": "none"}, clear=False), \
             patch.object(notify_backends, "_event_surface") as event, \
             patch.object(notify_backends, "_notify_send") as desktop:
            self.assertFalse(notify_backends.deliver("t", "b"))
        event.assert_not_called()
        desktop.assert_not_called()

        with patch.dict("os.environ", {}, clear=True), \
             patch.object(notify_backends.shutil, "which", return_value=None):
            self.assertFalse(notify_backends.deliver("t", "b"))


if __name__ == "__main__":
    unittest.main()
