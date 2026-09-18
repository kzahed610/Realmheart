"""Receipt-backed integrity view: drift is projected, never re-invented."""
from __future__ import annotations

import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path

from realmheart_doctor.cli import main


def _generated_receipt(root: Path):
    """Build a real accepted receipt over a live fixture installation."""
    from realmheart_installer.finalization import build_installed_state_receipt
    from tests.installer.test_verification_engine import Phase13VerificationTests

    helper = Phase13VerificationTests()
    helper.setUp()
    fixture_root = root / "fixture"
    paths, runner, plan, build = helper._fixture(fixture_root)
    environment = {
        "HOME": str(fixture_root / "home"),
        "XDG_CONFIG_HOME": str(fixture_root / "cfg"),
        "XDG_STATE_HOME": str(fixture_root / "state"),
        "PREFIX": str(fixture_root / "prefix"),
        "LIBEXEC": str(fixture_root / "prefix" / "libexec"),
        "SYSCONF": str(fixture_root / "etc"),
    }
    previous = {key: os.environ.get(key) for key in environment}
    os.environ.update(environment)
    try:
        report = helper._engine(paths, runner, plan, build).run()
        payload = build_installed_state_receipt(plan, report)
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
    path = root / "installed-state.json"
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path, payload, environment


@contextlib.contextmanager
def _environment(values):
    previous = {key: os.environ.get(key) for key in values}
    os.environ.update(values)
    try:
        yield
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def _invoke(*args):
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        code = main(list(args))
    return code, json.loads(output.getvalue())


class DoctorIntegrityTests(unittest.TestCase):
    def test_integrity_mode_requires_an_accepted_receipt(self):
        code, payload = _invoke("doctor", "--integrity", "--manifest-dir", "components", "--json")
        self.assertEqual(code, 3)
        self.assertEqual(payload["error"], "integrity_receipt_required")

    def test_component_and_report_flags_are_rejected_in_integrity_mode(self):
        code, payload = _invoke("doctor", "lockscreen-auth", "--integrity", "--json")
        self.assertEqual(code, 4)
        self.assertEqual(payload["error"], "invalid_invocation")

    def test_untouched_installation_has_no_error_or_critical_findings(self):
        with tempfile.TemporaryDirectory() as temp:
            receipt, _, environment = _generated_receipt(Path(temp))
            with _environment(environment):
                code, payload = _invoke(
                    "doctor", "--integrity", "--receipt", str(receipt),
                    "--manifest-dir", "components", "--json",
                )
        self.assertIn(payload["status"], {"clean", "attention"})
        self.assertIn(code, (0, 1))
        self.assertFalse([item for item in payload["findings"] if item["severity"] in {"critical", "error"}])

    def test_mutated_release_artifact_reports_drift(self):
        with tempfile.TemporaryDirectory() as temp:
            receipt, receipt_payload, environment = _generated_receipt(Path(temp))
            target = next(
                item for item in receipt_payload["artifacts"].values()
                if item["ownership"] == "release" and item["type"] == "executable"
            )
            path = Path(target["path"])
            path.write_bytes(path.read_bytes() + b"\n# tampered\n")
            with _environment(environment):
                code, payload = _invoke(
                    "doctor", "--integrity", "--receipt", str(receipt),
                    "--manifest-dir", "components", "--json",
                )
        self.assertEqual(code, 2)
        self.assertEqual(payload["status"], "drift")
        self.assertTrue([item for item in payload["findings"] if item["severity"] in {"critical", "error"}])


if __name__ == "__main__":
    unittest.main()
