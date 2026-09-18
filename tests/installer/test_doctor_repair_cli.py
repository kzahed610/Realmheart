"""Repair CLI: dry runs by default, explicit consent, verified outcomes."""
from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from realmheart_doctor.cli import main

_MANIFEST = '''schema_version = 1
release_version = "0.7.8"

[[components]]
id = "demo"
name = "Demo"
component_version = "release"
category = "core"
stage = "foundation"
repair_strategy_ids = ["rebuild_component", "install_missing_package"]

[[artifacts]]
id = "demo.binary"
component_id = "demo"
path = "$PREFIX/bin/demo"
type = "executable"
required = true
ownership = "release"
managed = true

[[health_checks]]
id = "demo.binary.exists"
component_id = "demo"
check = "artifact_exists"
artifact_id = "demo.binary"
contexts = ["doctor_manual"]

[[build_units]]
id = "demo-unit"
cmake_target = "demo_target"
component_ids = ["demo"]
artifact_ids = ["demo.binary"]

[[external_dependencies]]
id = "dep.runtime.grim"
name = "grim"

[[capabilities]]
id = "runtime.grim"
dependency_id = "dep.runtime.grim"
display_name = "grim"
requirement = "required"
lifecycle = ["runtime"]
component_id = "demo"
[capabilities.probe]
kind = "executable"
executable = "realmheart-definitely-missing-binary"
'''


def _invoke(*args):
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        code = main(list(args))
    return code, json.loads(output.getvalue())


class RepairCliTests(unittest.TestCase):
    def _manifest(self, root: Path) -> Path:
        directory = root / "components"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "demo.toml").write_text(_MANIFEST, encoding="utf-8")
        return directory

    def _installed_prefix(self, root: Path) -> Path:
        prefix = root / "prefix"
        (prefix / "bin").mkdir(parents=True, exist_ok=True)
        (prefix / "bin" / "demo").write_text("#!/bin/true\n", encoding="utf-8")
        return prefix

    def test_dry_run_prints_the_plan_without_executing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry_dir = self._manifest(root)
            code, payload = _invoke(
                "repair", "demo", "--prefix", str(root / "prefix"),
                "--manifest-dir", str(registry_dir), "--json",
            )
        self.assertEqual(code, 0)
        self.assertEqual(payload["mode"], "dry_run")
        action_types = [item["action_type"] for item in payload["plan"]["actions"]]
        self.assertIn("REBUILD_COMPONENT", action_types)
        self.assertEqual(action_types[-1], "RUN_POST_REPAIR_CHECKS")

    def test_unknown_component_is_invalid_invocation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry_dir = self._manifest(root)
            code, payload = _invoke("repair", "nope", "--manifest-dir", str(registry_dir), "--json")
        self.assertEqual(code, 4)
        self.assertEqual(payload["error"], "unknown component")

    def test_consent_refusal_is_exit_four_and_nothing_runs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry_dir = self._manifest(root)
            code, payload = _invoke(
                "repair", "demo", "--apply", "--prefix", str(root / "prefix"),
                "--manifest-dir", str(registry_dir), "--json",
            )
        self.assertEqual(code, 4)
        statuses = [item["status"] for item in payload["report"]["executions"]]
        self.assertIn("skipped_no_consent", statuses)

    def test_applied_repair_records_attempts_and_exits_unresolved(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry_dir = self._manifest(root)
            state = root / "state"
            code, payload = _invoke(
                "repair", "demo", "--apply", "--yes", "--prefix", str(root / "prefix"),
                "--state-dir", str(state), "--manifest-dir", str(registry_dir), "--json",
            )
            incidents = sorted(state.glob("incidents/RH-*.json"))
            attempts = json.loads(incidents[0].read_text()) if incidents else None
        self.assertEqual(code, 2)
        statuses = [item["status"] for item in payload["report"]["executions"]]
        self.assertEqual(statuses[0], "unavailable")
        self.assertEqual(statuses[-1], "unverified")
        self.assertIsNotNone(attempts)
        self.assertEqual(attempts["repair_attempts"][0]["status"], "unavailable")
        self.assertIn("REPAIR_ATTEMPTED", [event["event_type"] for event in attempts["timeline"]])

    def test_privileged_action_needs_the_explicit_flag(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry_dir = self._manifest(root)
            prefix = self._installed_prefix(root)
            code, payload = _invoke(
                "repair", "demo", "--apply", "--yes", "--prefix", str(prefix),
                "--manifest-dir", str(registry_dir), "--json",
            )
        self.assertEqual(code, 4)
        installs = [item for item in payload["report"]["executions"]
                    if item["action_type"] == "INSTALL_MISSING_PACKAGE"]
        self.assertTrue(installs)
        self.assertEqual(installs[0]["status"], "skipped_no_consent")

    def test_install_runner_refuses_without_a_terminal(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            registry_dir = self._manifest(root)
            prefix = self._installed_prefix(root)
            code, payload = _invoke(
                "repair", "demo", "--apply", "--yes", "--allow-privileged",
                "--prefix", str(prefix), "--manifest-dir", str(registry_dir), "--json",
            )
        self.assertIn(code, (2, 3))
        installs = [item for item in payload["report"]["executions"]
                    if item["action_type"] == "INSTALL_MISSING_PACKAGE"]
        self.assertEqual(installs[0]["status"], "refused")
        self.assertIn("terminal", installs[0]["detail"])

    def test_repair_modules_stay_installer_free_and_shell_free(self):
        import subprocess
        import sys

        procedural = subprocess.run(
            [sys.executable, "-B", "-c",
             "from realmheart_doctor.repair_runners import run_repair_plan; "
             "from realmheart_doctor.repair import plan_repairs; "
             "import sys; "
             "assert not any(k.startswith('realmheart_installer') for k in sys.modules)"],
            capture_output=True, text=True, timeout=20,
        )
        self.assertEqual(procedural.returncode, 0, procedural.stderr)
        boot = subprocess.run(
            [sys.executable, "-B", "-c",
             "from realmheart_doctor.boot import run_boot; "
             "import sys; "
             "assert 'realmheart_doctor.repair_runners' not in sys.modules; "
             "assert 'realmheart_doctor.repair' not in sys.modules"],
            capture_output=True, text=True, timeout=20,
        )
        self.assertEqual(boot.returncode, 0, boot.stderr)
        source = Path("realmheart_doctor/repair_runners.py").read_text(encoding="utf-8")
        self.assertNotIn("shell=True", source)


if __name__ == "__main__":
    unittest.main()
