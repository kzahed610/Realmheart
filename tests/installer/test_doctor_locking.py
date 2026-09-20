"""State locking prevents concurrent Doctor writers from corrupting state."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from realmheart_doctor.locking import acquire_state_lock


class LockingTests(unittest.TestCase):
    def test_second_boot_defers_while_first_holds_lock(self):
        from realmheart_doctor.boot import run_boot
        from realmheart_maintenance.manifest import load_manifest

        repo = Path(__file__).resolve().parents[2]
        registry = load_manifest(repo / "components")
        with tempfile.TemporaryDirectory() as temp:
            state_root = Path(temp) / "state"
            with acquire_state_lock(state_root):
                outcome = run_boot(registry, state_root, session_key="locked",
                                   notifier=lambda title, body, severity=None, **metadata: None)
                self.assertEqual(outcome.mode, "deferred_lock")
            self.assertFalse((state_root / "current.json").exists(),
                             "deferred boot must not touch state")

    def test_manual_diagnosis_waits_briefly_then_reports_contention(self):
        with tempfile.TemporaryDirectory() as temp:
            state_root = Path(temp) / "state"
            state_root.mkdir(parents=True)
            lock_path = state_root / ".lock"
            import fcntl

            held = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600)
            fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                with self.assertRaises(TimeoutError):
                    with acquire_state_lock(state_root, timeout=0.2):
                        pass
            finally:
                fcntl.flock(held, fcntl.LOCK_UN)
                os.close(held)

    def test_manual_stateful_doctor_reports_contention_without_mutating_state(self):
        import contextlib
        import io
        from unittest.mock import patch

        from realmheart_doctor.cli import main
        from realmheart_doctor.diagnosis import ComponentHealth
        from .test_doctor_incidents import _diagnosis

        with tempfile.TemporaryDirectory() as temp:
            state_root = Path(temp) / "state"
            with acquire_state_lock(state_root):
                output = io.StringIO()
                with patch("realmheart_doctor.diagnosis.diagnose",
                           return_value=_diagnosis(ComponentHealth.FAILED)) as diagnose, \
                     contextlib.redirect_stdout(output):
                    code = main(["doctor", "--state-dir", str(state_root), "--json"])
            payload = json.loads(output.getvalue())
            self.assertEqual(code, 5)
            self.assertEqual(payload["state"]["error"], "state_busy")
            diagnose.assert_not_called()
            self.assertFalse((state_root / "current.json").exists())

    def test_stateful_repair_reports_contention_before_running_repair(self):
        import contextlib
        import io
        from realmheart_doctor.cli import main

        with tempfile.TemporaryDirectory() as temp:
            state_root = Path(temp) / "state"
            with acquire_state_lock(state_root):
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    code = main([
                        "repair", "screenshot", "--apply", "--state-dir", str(state_root),
                        "--manifest-dir", "components", "--json",
                    ])
            payload = json.loads(output.getvalue())
            self.assertEqual(code, 5)
            self.assertEqual(payload["error"], "state_busy")
            self.assertFalse((state_root / "current.json").exists())

    def test_lock_is_released_on_context_exit_and_crash_safety(self):
        with tempfile.TemporaryDirectory() as temp:
            state_root = Path(temp) / "state"
            with acquire_state_lock(state_root, timeout=1):
                pass
            # A fresh acquire in-process proves release.
            with acquire_state_lock(state_root, timeout=1):
                pass
            self.assertTrue((state_root / ".lock").is_file())

    def test_lock_survives_sibling_process_probe(self):
        with tempfile.TemporaryDirectory() as temp:
            state_root = Path(temp) / "state"
            child = (
                "from realmheart_doctor.locking import acquire_state_lock\n"
                "import sys, time\n"
                "from pathlib import Path\n"
                "with acquire_state_lock(Path(sys.argv[1]), timeout=5):\n"
                "    print('held', flush=True)\n"
                "    time.sleep(2)\n"
            )
            process = subprocess.Popen([sys.executable, "-c", child, str(state_root)],
                                       stdout=subprocess.PIPE, text=True)
            try:
                self.assertEqual(process.stdout.readline().strip(), "held")
                with self.assertRaises(TimeoutError):
                    with acquire_state_lock(state_root, timeout=0.3):
                        pass
            finally:
                process.wait(timeout=10)
