from __future__ import annotations

import io
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from . import _bootstrap  # noqa: F401
from realmheart_installer.cli import main


class DryRunCliTests(unittest.TestCase):
    def test_dry_run_does_not_create_persistent_transaction_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root / "home"
            runtime = root / "run"
            for path in (home, runtime):
                path.mkdir(parents=True)
            env = {
                "HOME": str(home),
                "XDG_CONFIG_HOME": str(root / "cfg"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(runtime),
            }
            fake_snapshot = object()
            fake_plan = SimpleNamespace(ready=True)
            output = io.StringIO()
            with patch.dict(os.environ, env, clear=False), \
                 patch("realmheart_installer.cli.ensure_not_root"), \
                 patch("realmheart_installer.cli.PreflightScanner") as scanner_cls, \
                 patch("realmheart_installer.cli.InstallationPlanner") as planner_cls, \
                 patch("realmheart_installer.cli.InstallContext.create", side_effect=AssertionError("dry-run must not create InstallContext")), \
                 patch("realmheart_installer.cli.render_installation_plan", return_value="DRY PLAN"), \
                 patch("sys.stdout", output):
                scanner_cls.return_value.scan.return_value = fake_snapshot
                planner_cls.return_value.build.return_value = fake_plan
                old = Path.cwd()
                try:
                    os.chdir(_bootstrap.REPO_ROOT)
                    rc = main(["--dry-run"])
                finally:
                    os.chdir(old)
            self.assertEqual(rc, 0)
            self.assertIn("DRY PLAN", output.getvalue())
            self.assertFalse((root / "state/realmheart-installer/transactions").exists())
            self.assertTrue((runtime / "realmheart-installer.lock").exists())

    def test_uninstall_dry_run_is_read_only_and_skips_preflight(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root / "home"
            runtime = root / "run"
            for path in (home, runtime):
                path.mkdir(parents=True)
            env = {
                "HOME": str(home),
                "XDG_CONFIG_HOME": str(root / "cfg"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(runtime),
            }
            fake_plan = SimpleNamespace(ready=True)
            output = io.StringIO()
            with patch.dict(os.environ, env, clear=False), \
                 patch("realmheart_installer.cli.ensure_not_root"), \
                 patch("realmheart_installer.cli.PreflightScanner", side_effect=AssertionError("uninstall dry-run must skip install preflight")), \
                 patch("realmheart_installer.cli.InstallContext.create", side_effect=AssertionError("uninstall dry-run must not create InstallContext")), \
                 patch("realmheart_installer.cli.UninstallPlanner") as planner_cls, \
                 patch("realmheart_installer.cli.render_uninstall_plan", return_value="UNINSTALL DRY PLAN"), \
                 patch("sys.stdout", output):
                planner_cls.return_value.build.return_value = fake_plan
                old = Path.cwd()
                try:
                    os.chdir(_bootstrap.REPO_ROOT)
                    rc = main(["--dry-run", "uninstall"])
                finally:
                    os.chdir(old)
            self.assertEqual(rc, 0)
            self.assertIn("UNINSTALL DRY PLAN", output.getvalue())
            self.assertFalse((root / "state/realmheart-installer/transactions").exists())
            self.assertTrue((runtime / "realmheart-installer.lock").exists())

    def test_uninstall_compare_is_read_only_without_dry_run_flag(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root / "home"
            runtime = root / "run"
            for path in (home, runtime):
                path.mkdir(parents=True)
            env = {
                "HOME": str(home),
                "XDG_CONFIG_HOME": str(root / "cfg"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(runtime),
            }
            fake_plan = SimpleNamespace(ready=True, comparisons=())
            output = io.StringIO()
            with patch.dict(os.environ, env, clear=False), \
                 patch("realmheart_installer.cli.ensure_not_root"), \
                 patch("realmheart_installer.cli.PreflightScanner", side_effect=AssertionError("uninstall compare must skip install preflight")), \
                 patch("realmheart_installer.cli.InstallContext.create", side_effect=AssertionError("uninstall compare must not create InstallContext")), \
                 patch("realmheart_installer.cli.UninstallPlanner") as planner_cls, \
                 patch("realmheart_installer.cli.render_uninstall_compare", return_value="UNINSTALL COMPARE"), \
                 patch("sys.stdout", output):
                planner_cls.return_value.build.return_value = fake_plan
                old = Path.cwd()
                try:
                    os.chdir(_bootstrap.REPO_ROOT)
                    rc = main(["--compare-config", "uninstall"])
                finally:
                    os.chdir(old)
            self.assertEqual(rc, 0)
            self.assertIn("UNINSTALL COMPARE", output.getvalue())
            self.assertFalse((root / "state/realmheart-installer/transactions").exists())

    def test_component_plan_inspection_is_non_mutating_in_dry_run(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root / "home"
            runtime = root / "run"
            for path in (home, runtime):
                path.mkdir(parents=True)
            env = {
                "HOME": str(home),
                "XDG_CONFIG_HOME": str(root / "cfg"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(runtime),
            }
            fake_snapshot = object()
            fake_plan = SimpleNamespace(ready=True)
            output = io.StringIO()
            with patch.dict(os.environ, env, clear=False), \
                 patch("realmheart_installer.cli.ensure_not_root"), \
                 patch("realmheart_installer.cli.PreflightScanner") as scanner_cls, \
                 patch("realmheart_installer.cli.InstallationPlanner") as planner_cls, \
                 patch("realmheart_installer.cli.InstallContext.create", side_effect=AssertionError("dry-run must not create InstallContext")), \
                 patch("realmheart_installer.cli.resolve_component_handler_specs", return_value=()), \
                 patch("realmheart_installer.cli.render_component_footprints", return_value="[01/18] Realmheart Core"), \
                 patch("sys.stdout", output):
                scanner_cls.return_value.scan.return_value = fake_snapshot
                planner_cls.return_value.build.return_value = fake_plan
                old = Path.cwd()
                try:
                    os.chdir(_bootstrap.REPO_ROOT)
                    rc = main(["--dry-run", "component-plan"])
                finally:
                    os.chdir(old)
            self.assertEqual(rc, 0)
            self.assertIn("Realmheart Core", output.getvalue())
            self.assertIn("Inspection only", output.getvalue())
            self.assertFalse((root / "state/realmheart-installer/transactions").exists())


    def test_verify_current_is_observational_without_dry_run_flag(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root / "home"
            runtime = root / "run"
            for path in (home, runtime):
                path.mkdir(parents=True)
            env = {
                "HOME": str(home),
                "XDG_CONFIG_HOME": str(root / "cfg"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(runtime),
            }
            fake_snapshot = SimpleNamespace(capabilities=())
            fake_plan = SimpleNamespace(ready=True)
            fake_report = SimpleNamespace(ok=True)
            output = io.StringIO()
            with patch.dict(os.environ, env, clear=False), \
                 patch("realmheart_installer.cli.ensure_not_root"), \
                 patch("realmheart_installer.cli.PreflightScanner") as scanner_cls, \
                 patch("realmheart_installer.cli.InstallationPlanner") as planner_cls, \
                 patch("realmheart_installer.cli.InstallContext.create", side_effect=AssertionError("verify-current must not create InstallContext")), \
                 patch("realmheart_installer.cli.VerificationEngine") as verify_cls, \
                 patch("realmheart_installer.cli.render_verification_report", return_value="VERIFY REPORT"), \
                 patch("sys.stdout", output):
                scanner_cls.return_value.scan.return_value = fake_snapshot
                planner_cls.return_value.build.return_value = fake_plan
                verify_cls.return_value.run.return_value = fake_report
                old = Path.cwd()
                try:
                    os.chdir(_bootstrap.REPO_ROOT)
                    rc = main(["verify-current"])
                finally:
                    os.chdir(old)
            self.assertEqual(rc, 0)
            self.assertIn("VERIFY REPORT", output.getvalue())
            self.assertIn("Observation only", output.getvalue())
            self.assertFalse((root / "state/realmheart-installer/transactions").exists())

    def test_report_commands_skip_preflight_and_use_incident_ids(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root / "home"
            runtime = root / "run"
            for path in (home, runtime):
                path.mkdir(parents=True)
            env = {
                "HOME": str(home),
                "XDG_CONFIG_HOME": str(root / "cfg"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(runtime),
            }
            incident = "RH-DIAG-20260913-040000-ABCDEF12-1234"
            bundle = SimpleNamespace(incident_id=incident, directory=root / "reports" / incident)
            with patch.dict(os.environ, env, clear=False), \
                 patch("realmheart_installer.cli.ensure_not_root"), \
                 patch("realmheart_installer.cli.PreflightScanner", side_effect=AssertionError("report commands must skip preflight")), \
                 patch("realmheart_installer.cli.DiagnosticReportStore") as store_cls:
                store_cls.return_value.list.return_value = (bundle,)
                out = io.StringIO()
                with patch("sys.stdout", out):
                    self.assertEqual(main(["report-list"]), 0)
                self.assertIn(incident, out.getvalue())

                store_cls.return_value.inspect.return_value = (bundle, {"incident_id": incident, "schema_version": 1}, "REPORT BODY\n")
                out = io.StringIO()
                with patch("sys.stdout", out):
                    self.assertEqual(main(["report-inspect", "--report-id", incident]), 0)
                self.assertIn("REPORT BODY", out.getvalue())

                out = io.StringIO()
                with patch("sys.stdout", out):
                    self.assertEqual(main(["report-remove", "--report-id", incident]), 0)
                store_cls.return_value.remove.assert_called_with(incident)

    def test_diagnose_current_dry_run_does_not_persist_report_or_transaction(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            home = root / "home"
            runtime = root / "run"
            for path in (home, runtime):
                path.mkdir(parents=True)
            env = {
                "HOME": str(home),
                "XDG_CONFIG_HOME": str(root / "cfg"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(runtime),
            }
            fake_snapshot = SimpleNamespace(capabilities=())
            fake_plan = SimpleNamespace(ready=True)
            fake_verify = SimpleNamespace(ok=True)
            fake_diag = SimpleNamespace(has_failures=False)
            output = io.StringIO()
            with patch.dict(os.environ, env, clear=False), \
                 patch("realmheart_installer.cli.ensure_not_root"), \
                 patch("realmheart_installer.cli.PreflightScanner") as scanner_cls, \
                 patch("realmheart_installer.cli.InstallationPlanner") as planner_cls, \
                 patch("realmheart_installer.cli.InstallContext.create", side_effect=AssertionError("diagnose dry-run must not create InstallContext")), \
                 patch("realmheart_installer.cli.VerificationEngine") as verify_cls, \
                 patch("realmheart_installer.cli.DiagnosticReportBuilder") as diag_cls, \
                 patch("realmheart_installer.cli.DiagnosticReportStore") as store_cls, \
                 patch("realmheart_installer.cli.render_markdown_report", return_value="DIAGNOSTIC\n"), \
                 patch("sys.stdout", output):
                scanner_cls.return_value.scan.return_value = fake_snapshot
                planner_cls.return_value.build.return_value = fake_plan
                verify_cls.return_value.run.return_value = fake_verify
                diag_cls.return_value.build.return_value = fake_diag
                old = Path.cwd()
                try:
                    os.chdir(_bootstrap.REPO_ROOT)
                    rc = main(["--dry-run", "diagnose-current"])
                finally:
                    os.chdir(old)
            self.assertEqual(rc, 0)
            self.assertIn("Dry-run: diagnostic bundle was not persisted", output.getvalue())
            store_cls.return_value.save.assert_not_called()
            self.assertFalse((root / "state/realmheart-installer/transactions").exists())


if __name__ == "__main__":
    unittest.main()
