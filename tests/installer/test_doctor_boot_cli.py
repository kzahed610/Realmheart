"""Boot-mode CLI entry: noninteractive one-shot over the shared state contract."""
import unittest


class BootCLITests(unittest.TestCase):
    def test_boot_mode_records_state_and_honors_session_marker(self):
        import json
        import os
        import subprocess
        import tempfile
        from pathlib import Path

        repo = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as temp:
            state = str(Path(temp) / "state")
            def run(session: str) -> dict:
                import subprocess
                process = subprocess.run(
                    [str(repo / "tools" / "realmheart-doctor.py"), "boot", "--state-dir", state,
                     "--session-key", session, "--json"],
                    cwd=repo, capture_output=True, text=True, timeout=300,
                    env={**os.environ, "REALMHEART_DOCTOR_NOTIFY_BACKEND": "none"})
                self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
                return json.loads(process.stdout)
            first = run("boot-cli-fixture")
            self.assertEqual(first["mode"], "ran")
            second = run("boot-cli-fixture")
            self.assertEqual(second["mode"], "already_ran")
            self.assertTrue((Path(state) / "current.json").is_file())

    def test_boot_mode_without_notification_backend_uses_noop(self):
        import io
        import contextlib
        import json
        import os
        import tempfile
        from pathlib import Path
        from unittest.mock import patch

        from realmheart_doctor.cli import main

        with tempfile.TemporaryDirectory() as temp:
            with patch.dict(os.environ, {"REALMHEART_DOCTOR_NOTIFY_BACKEND": "none"}), \
                 contextlib.redirect_stdout(io.StringIO()) as output:
                code = main(["boot", "--state-dir", temp, "--session-key", "fixture", "--json"])
            self.assertEqual(code, 0)
            payload = json.loads(output.getvalue())
            self.assertEqual(payload["mode"], "ran")
            self.assertEqual(payload["notifications"], 0)
            incidents = list((Path(temp) / "incidents").glob("RH-*.json"))
            self.assertTrue(incidents)
            for path in incidents:
                self.assertNotIn("last_notified_state", json.loads(path.read_text()),
                                 "undelivered incident must stay eligible for retry")

    def test_no_notify_flag_still_allows_later_delivery(self):
        import io
        import contextlib
        import json
        import tempfile
        from pathlib import Path
        from unittest.mock import patch

        from realmheart_doctor.cli import main
        from .test_doctor_state import _diagnosis
        from realmheart_doctor.diagnosis import ComponentHealth

        with tempfile.TemporaryDirectory() as temp:
            with patch("realmheart_doctor.boot.diagnose", return_value=_diagnosis(ComponentHealth.FAILED)), \
                 contextlib.redirect_stdout(io.StringIO()) as output:
                code = main(["boot", "--state-dir", temp, "--session-key", "fixture-quiet",
                             "--no-notify", "--json"])
            self.assertEqual(code, 0)
            self.assertEqual(json.loads(output.getvalue())["notifications"], 0)
            incidents = list((Path(temp) / "incidents").glob("RH-*.json"))
            self.assertTrue(incidents)
            for path in incidents:
                self.assertNotIn("last_notified_state", json.loads(path.read_text()))

    def test_boot_without_a_session_key_falls_back_to_the_machine_boot_id(self):
        import contextlib
        import io
        import json
        import os
        import tempfile
        from unittest.mock import patch

        from realmheart_doctor.cli import main

        without_signature = {key: value for key, value in os.environ.items()
                             if key != "HYPRLAND_INSTANCE_SIGNATURE"}
        with tempfile.TemporaryDirectory() as temp:
            with patch.dict(os.environ, {**without_signature,
                                         "REALMHEART_DOCTOR_NOTIFY_BACKEND": "none"}, clear=True), \
                 contextlib.redirect_stdout(io.StringIO()) as output:
                code = main(["boot", "--state-dir", temp, "--json"])
                second = main(["boot", "--state-dir", temp, "--json"])
            lines = output.getvalue().splitlines()
        self.assertEqual(code, 0)
        self.assertEqual(second, 0)
        self.assertEqual(json.loads(lines[0])["mode"], "ran")
        self.assertEqual(json.loads(lines[1])["mode"], "already_ran")

    def test_notify_backend_delivers_actionable_title(self):
        import io
        import contextlib
        import json
        import tempfile
        from pathlib import Path
        from unittest.mock import patch

        from realmheart_doctor.cli import main
        from .test_doctor_state import _diagnosis
        from realmheart_doctor.diagnosis import ComponentHealth

        calls = []

        def deliver(title, body, severity="warning", **metadata):
            calls.append((title, severity, metadata))
            return True

        with tempfile.TemporaryDirectory() as temp:
            with patch("realmheart_doctor.boot.diagnose", return_value=_diagnosis(ComponentHealth.FAILED)), \
                 patch("realmheart_doctor.notify_backends.deliver", side_effect=deliver), \
                 contextlib.redirect_stdout(io.StringIO()) as output:
                code = main(["boot", "--state-dir", temp, "--session-key", "fixture2", "--json"])
            self.assertEqual(code, 0)
            self.assertEqual(len(calls), 1)
            self.assertIn("Realmheart", calls[0][0])
            self.assertEqual(calls[0][1], "critical")
            self.assertTrue(calls[0][2]["incident_id"].startswith("RH-"))
            self.assertIn("repair_available", calls[0][2])
            self.assertEqual(json.loads(output.getvalue())["notifications"], 1)
