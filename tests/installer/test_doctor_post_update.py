"""Post-update correlation: bounded, evidence-only, one-shot marker."""
from __future__ import annotations

import json
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import Mock

from realmheart_maintenance.manifest import load_manifest
from realmheart_doctor.health import HealthCheckReport
from realmheart_doctor.post_update import (
    correlate_package_updates,
    pending_window_start,
    record_package_updates,
    relevant_packages,
    run_post_update,
)

def _log_line(package: str, previous: str, current: str, at: datetime) -> str:
    return f"[{at.strftime('%Y-%m-%dT%H:%M:%S%z')}] [ALPM] upgraded {package} ({previous} -> {current})"


def _fixture(root: Path, *, marker_age_seconds: int = 60):
    import os

    now = datetime.now(timezone.utc)
    marker_at = now - timedelta(seconds=marker_age_seconds)
    marker = root / "post-update.pending"
    marker.write_text("")
    os.utime(marker, (marker_at.timestamp(), marker_at.timestamp()))
    inside = marker_at + timedelta(seconds=5)
    log = root / "pacman.log"
    log.write_text("\n".join((
        _log_line("gtk4", "4.20.1-1", "4.22.4-1", inside),
        _log_line("some-unrelated-package", "1.0-1", "1.1-1", inside),
        _log_line("gtk4-layer-shell", "1.1.0-1", "1.3.0-1", inside),
    )) + "\n", encoding="utf-8")
    return marker, log


class PostUpdateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.registry = load_manifest(Path("components"))

    def test_relevant_packages_map_back_to_components(self) -> None:
        mapping = relevant_packages(self.registry)
        self.assertEqual(mapping["gtk4"], ("realmheart-core",))
        self.assertEqual(mapping["tesseract"], ("screenshot-ocr",))
        self.assertNotIn("some-unrelated-package", mapping)

    def test_correlation_is_bounded_to_the_marker_window(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            marker, log = _fixture(root, marker_age_seconds=3600)
            report = correlate_package_updates(
                self.registry, root / "state", marker_path=marker, log_path=log,
            )
        self.assertIsNotNone(report)
        packages = [item["package"] for item in report.transactions]
        self.assertEqual(packages, ["gtk4", "gtk4-layer-shell"])
        self.assertIn("realmheart-core", report.affected_components)
        self.assertFalse(report.marker_consumed)
        self.assertTrue(all(item["proves_causation"] is False for item in report.transactions))

    def test_old_transactions_outside_the_window_are_ignored(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            marker, log = _fixture(root)
            stale = datetime.now(timezone.utc) - timedelta(days=5)
            log.write_text(
                _log_line("gtk4", "4.20.1-1", "4.22.4-1", stale) + "\n",
                encoding="utf-8",
            )
            report = correlate_package_updates(
                self.registry, root / "state", marker_path=marker, log_path=log,
            )
        self.assertIsNotNone(report)
        self.assertEqual(report.transactions, ())

    def test_consumed_marker_is_not_replayed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = root / "state"
            marker, log = _fixture(root)
            report = correlate_package_updates(self.registry, state, marker_path=marker, log_path=log)
            record_package_updates(state, report, marker_path=marker)
            self.assertIsNone(pending_window_start(state, marker_path=marker))
            self.assertIsNone(correlate_package_updates(self.registry, state, marker_path=marker, log_path=log))

    def test_missing_marker_and_unreadable_log_are_reported_honestly(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            outcome = run_post_update(self.registry, root / "state",
                                      marker_path=root / "absent.pending", log_path=root / "absent.log")
            self.assertEqual(outcome.mode, "no_update_marker")
            marker, _ = _fixture(root)
            outcome = run_post_update(self.registry, root / "state",
                                      marker_path=marker, log_path=root / "absent.log")
            self.assertEqual(outcome.mode, "log_unavailable")
            self.assertFalse((root / "state" / "post-update.json").exists())

    def test_relevant_update_runs_a_bounded_diagnosis_and_records_it(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = root / "state"
            marker, log = _fixture(root)
            executor = Mock()
            executor.execute.return_value = HealthCheckReport((), 0)
            outcome = run_post_update(
                self.registry, state, executor=executor, marker_path=marker, log_path=log,
                notifier=lambda title, body, severity=None: None,
            )
            record = json.loads((state / "post-update.json").read_text(encoding="utf-8"))
        self.assertEqual(outcome.mode, "ran")
        self.assertTrue(outcome.report.marker_consumed)
        self.assertEqual(record["transactions"][0]["package"], "gtk4")
        self.assertEqual(executor.execute.call_args.kwargs["context"], "doctor_background")
        self.assertEqual(executor.execute.call_args.kwargs["max_cost"], "cheap")


    def test_targeted_update_runs_only_affected_component_and_required_upstreams(self) -> None:
        import os

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = root / "state"
            marker = root / "post-update.pending"
            marker.write_text("")
            now = datetime.now(timezone.utc)
            marker_at = now - timedelta(seconds=30)
            os.utime(marker, (marker_at.timestamp(), marker_at.timestamp()))
            log = root / "pacman.log"
            log.write_text(
                _log_line("tesseract", "5.5.1-1", "5.6.0-1", marker_at + timedelta(seconds=2)) + "\n",
                encoding="utf-8",
            )
            executor = Mock()
            executor.execute.return_value = HealthCheckReport((), 0)
            outcome = run_post_update(
                self.registry, state, executor=executor, marker_path=marker, log_path=log, now=now,
            )
        self.assertEqual(outcome.mode, "ran")
        self.assertEqual(outcome.report.affected_components, ("screenshot-ocr",))
        check_ids = executor.execute.call_args.kwargs["check_ids"]
        checked_components = {self.registry.health_checks[item].component_id for item in check_ids}
        self.assertEqual(checked_components, {"realmheart-core", "screenshot"})
        self.assertLess(len(check_ids), len(self.registry.health_checks))

    def test_irrelevant_update_records_without_re_checking(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = root / "state"
            marker = root / "post-update.pending"
            marker.write_text("")
            log = root / "pacman.log"
            log.write_text("[2026-09-18T10:00:00+0000] [ALPM] upgraded unrelated (1 -> 2)\n", encoding="utf-8")
            executor = Mock()
            outcome = run_post_update(self.registry, state, executor=executor, marker_path=marker, log_path=log)
        self.assertEqual(outcome.mode, "no_relevant_changes")
        self.assertFalse(executor.execute.called)

    def test_cli_post_update_is_a_quiet_noop_without_a_marker(self) -> None:
        import contextlib
        import io

        from realmheart_doctor.cli import main

        output = io.StringIO()
        with tempfile.TemporaryDirectory() as temp:
            with contextlib.redirect_stdout(output):
                code = main(["post-update", "--state-dir", temp, "--manifest-dir", "components", "--json"])
        payload = json.loads(output.getvalue())
        self.assertEqual(code, 0)
        self.assertEqual(payload["mode"], "no_update_marker")

    def test_cli_post_update_rejects_an_invalid_since(self) -> None:
        import contextlib
        import io

        from realmheart_doctor.cli import main

        output = io.StringIO()
        with tempfile.TemporaryDirectory() as temp:
            with contextlib.redirect_stdout(output):
                code = main(["post-update", "--state-dir", temp, "--manifest-dir", "components",
                             "--since", "not-a-timestamp", "--json"])
        self.assertEqual(code, 4)
        self.assertEqual(json.loads(output.getvalue())["error"], "invalid_invocation")

    def test_pacman_integration_stays_tiny_and_privilege_free(self) -> None:
        from . import _bootstrap

        hook = (_bootstrap.REPO_ROOT / "config/pacman/hooks/10-realmheart-doctor.hook.in").read_text(encoding="utf-8")
        marker = (_bootstrap.REPO_ROOT / "config/pacman/realmheart-post-update-marker.sh").read_text(encoding="utf-8")
        self.assertIn("When = PostTransaction", hook)
        self.assertIn("@REALMHEART_MARKER_EXECUTABLE@", hook)
        for script in (hook, marker):
            code = "\n".join(line for line in script.splitlines() if not line.lstrip().startswith("#"))
            self.assertNotIn("sudo", code)
            self.assertNotIn("pacman -", code)
            self.assertNotIn("realmheart-doctor", code)
        self.assertIn("post-update.pending", marker)

    def test_post_update_defers_without_consuming_marker_when_state_is_locked(self) -> None:
        from realmheart_doctor.locking import acquire_state_lock

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = root / "state"
            marker, log = _fixture(root)
            with acquire_state_lock(state):
                outcome = run_post_update(
                    self.registry, state, marker_path=marker, log_path=log, lock_timeout=0.0,
                )
            self.assertEqual(outcome.mode, "deferred_lock")
            self.assertFalse((state / "post-update.json").exists())
            self.assertIsNotNone(pending_window_start(state, marker_path=marker))

    def test_post_update_runs_retention_inside_the_state_transaction(self) -> None:
        import os

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            state = root / "state"
            incidents = state / "incidents"
            incidents.mkdir(parents=True)
            for index in range(21):
                day = f"202608{index + 1:02d}"
                (incidents / f"RH-{day}-001.json").write_text(json.dumps({
                    "format_version": 1, "id": f"RH-{day}-001",
                    "component_id": "demo", "resolution_state": "resolved", "timeline": [],
                }))
            marker = root / "post-update.pending"
            marker.write_text("")
            now = datetime.now(timezone.utc)
            marker_at = now - timedelta(seconds=30)
            os.utime(marker, (marker_at.timestamp(), marker_at.timestamp()))
            log = root / "pacman.log"
            log.write_text(
                _log_line("unrelated", "1", "2", marker_at + timedelta(seconds=1)) + "\n",
                encoding="utf-8",
            )
            outcome = run_post_update(
                self.registry, state, marker_path=marker, log_path=log, now=now,
            )
            self.assertEqual(outcome.mode, "no_relevant_changes")
            self.assertEqual(len(list(incidents.glob("RH-*.json"))), 20)

    def test_manifest_declares_the_post_update_integration(self) -> None:
        hook = self.registry.artifacts["doctor.pacman-hook"]
        marker = self.registry.artifacts["doctor.update-marker"]
        self.assertTrue(hook.required)
        self.assertTrue(marker.required)
        self.assertEqual(marker.mode, "0755")
        self.assertEqual(hook.ownership, "system")
        checks = {spec.artifact_id: spec for spec in self.registry.health_checks.values()}
        self.assertIn("doctor.pacman-hook", checks)
        self.assertIn("doctor.update-marker", checks)
        self.assertIn("install_verify", checks["doctor.pacman-hook"].contexts)


if __name__ == "__main__":
    unittest.main()
