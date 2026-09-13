from __future__ import annotations

import io
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from contextlib import nullcontext
from types import SimpleNamespace
from unittest.mock import patch

from . import _bootstrap  # noqa: F401
from realmheart_installer.cli import main
from realmheart_installer.models import TransactionState


class _FakeTransaction:
    def __init__(self) -> None:
        self.state = TransactionState.CREATED
        self.metadata: dict[str, object] = {}
        self.install_mode = None
        self.installation_origin = None
        self.current_version = None
        self.target_version = None

    def transition(self, state: TransactionState) -> None:
        self.state = state


class _FakeContext:
    def __init__(self) -> None:
        self.transaction = _FakeTransaction()
        self.persisted = 0
        self.persisted_json: list[str] = []
        self.reserve_released = False

    def persist_summary(self) -> None:
        self.persisted += 1

    def persist_json(self, filename: str, _payload) -> Path:
        self.persisted_json.append(filename)
        return Path(filename)

    def release_recovery_reserve(self) -> None:
        self.reserve_released = True


def _snapshot(*, package_manager=None):
    return SimpleNamespace(
        ready=True,
        capabilities=(),
        package_manager=package_manager or SimpleNamespace(kind="pacman", automatic_dependency_install=True),
        installation=SimpleNamespace(
            mode=SimpleNamespace(value="fresh"),
            origin=SimpleNamespace(value="none"),
            installed_version_text=None,
            source=SimpleNamespace(version_text="0.7.8"),
        ),
    )


def _plan(*, ready: bool, package_actions=(), package_plan=None):
    return SimpleNamespace(
        ready=ready,
        plan_digest="plan-digest",
        manifest_digest="manifest-digest",
        package_actions=package_actions,
        package_plan=package_plan or SimpleNamespace(packages=(), mutation_blockers=(), unresolved=()),
    )


class ReleaseAuditTests(unittest.TestCase):
    def test_preflight_is_read_only_and_resolves_source_from_entrypoint_not_cwd(self) -> None:
        with tempfile.TemporaryDirectory(dir="/dev/shm") as temp:
            root = Path(temp)
            home = root / "home"
            runtime = root / "run"
            elsewhere = root / "elsewhere"
            for path in (home, runtime, elsewhere):
                path.mkdir(parents=True)
            env = {
                "HOME": str(home),
                "XDG_CONFIG_HOME": str(root / "cfg"),
                "XDG_STATE_HOME": str(root / "state"),
                "XDG_DATA_HOME": str(root / "data"),
                "XDG_CACHE_HOME": str(root / "cache"),
                "XDG_RUNTIME_DIR": str(runtime),
            }
            scanner_snapshot = _snapshot()
            with patch.dict(os.environ, env, clear=False), \
                 patch("realmheart_installer.cli.ensure_not_root"), \
                 patch("realmheart_installer.cli.InstallContext.create", side_effect=AssertionError("preflight must not create transaction state")), \
                 patch("realmheart_installer.cli.PreflightScanner") as scanner_cls, \
                 patch("realmheart_installer.cli.render_preflight", return_value="PREFLIGHT"), \
                 patch("sys.stdout", io.StringIO()):
                scanner_cls.return_value.scan.return_value = scanner_snapshot
                old = Path.cwd()
                try:
                    os.chdir(elsewhere)
                    rc = main(["preflight"])
                finally:
                    os.chdir(old)
            self.assertEqual(rc, 0)
            kwargs = scanner_cls.call_args.kwargs
            self.assertEqual(kwargs["source_root"], _bootstrap.REPO_ROOT)
            self.assertFalse((root / "state/realmheart-installer/transactions").exists())

    def test_default_plan_and_dependency_inspection_do_not_create_transactions(self) -> None:
        snapshot = _snapshot()
        plan = _plan(ready=True)
        with patch("realmheart_installer.cli.ensure_not_root"), \
             patch("realmheart_installer.cli.InstallerLock", side_effect=lambda *args, **kwargs: nullcontext()), \
             patch("realmheart_installer.cli.PreflightScanner") as scanner_cls, \
             patch("realmheart_installer.cli.InstallationPlanner") as planner_cls, \
             patch("realmheart_installer.cli.InstallContext.create", side_effect=AssertionError("read-only inspection created a transaction")), \
             patch("realmheart_installer.cli.render_installation_plan", return_value="PLAN"), \
             patch("sys.stdout", io.StringIO()):
            scanner_cls.return_value.scan.return_value = snapshot
            planner_cls.return_value.build.return_value = plan
            self.assertEqual(main([]), 0)

        with patch("realmheart_installer.cli.ensure_not_root"), \
             patch("realmheart_installer.cli.InstallerLock", side_effect=lambda *args, **kwargs: nullcontext()), \
             patch("realmheart_installer.cli.PreflightScanner") as scanner_cls, \
             patch("realmheart_installer.cli.InstallContext.create", side_effect=AssertionError("dependency inspection created a transaction")), \
             patch("realmheart_installer.cli._run_dependencies", return_value=0) as dependency_run:
            scanner_cls.return_value.scan.return_value = snapshot
            self.assertEqual(main(["dependencies"]), 0)
            self.assertIsNone(dependency_run.call_args.args[2])

    def test_dry_run_cannot_mask_mutating_recovery_or_report_commands(self) -> None:
        for argv, forbidden in (
            (["--dry-run", "recovery-rollback", "--transaction-id", "RH-TEST"], "recover_transaction_from_journal"),
            (["--dry-run", "report-remove", "--report-id", "RH-DIAG-TEST"], "DiagnosticReportStore"),
        ):
            with self.subTest(argv=argv), \
                 patch("realmheart_installer.cli.ensure_not_root") as root_check, \
                 patch(f"realmheart_installer.cli.{forbidden}") as dangerous, \
                 patch("sys.stderr", io.StringIO()) as stderr:
                self.assertEqual(main(argv), 2)
                root_check.assert_not_called()
                dangerous.assert_not_called()
                self.assertIn("RH_CLI_FLAG_CONFLICT", stderr.getvalue())

    def test_blocked_live_install_closes_transaction_before_mutation(self) -> None:
        context = _FakeContext()
        snapshot = _snapshot()
        plan = _plan(ready=False)
        with patch("realmheart_installer.cli.ensure_not_root"), \
             patch("realmheart_installer.cli.InstallerLock", side_effect=lambda *args, **kwargs: nullcontext()), \
             patch("realmheart_installer.cli.discover_recovery_candidates", return_value=()), \
             patch("realmheart_installer.cli.InstallContext.create", return_value=context), \
             patch("realmheart_installer.cli.PreflightScanner") as scanner_cls, \
             patch("realmheart_installer.cli.InstallationPlanner") as planner_cls, \
             patch("realmheart_installer.cli.render_installation_plan", return_value="BLOCKED PLAN"), \
             patch("sys.stdout", io.StringIO()):
            scanner_cls.return_value.scan.return_value = snapshot
            planner_cls.return_value.build.return_value = plan
            self.assertEqual(main(["install"]), 12)
        self.assertEqual(context.transaction.state, TransactionState.FAILED)
        self.assertTrue(context.transaction.metadata["plan_blocked"])
        self.assertFalse(context.transaction.metadata["live_mutation_started"])
        self.assertTrue(context.reserve_released)

    def test_dependency_and_live_consent_occurs_before_pacman_mutation(self) -> None:
        context = _FakeContext()
        snapshot = _snapshot()
        package_plan = SimpleNamespace(packages=("pkg-one",), mutation_blockers=(), unresolved=())
        plan = _plan(ready=True, package_actions=("pkg-one",), package_plan=package_plan)
        output = io.StringIO()
        with patch("realmheart_installer.cli.ensure_not_root"), \
             patch("realmheart_installer.cli.InstallerLock", side_effect=lambda *args, **kwargs: nullcontext()), \
             patch("realmheart_installer.cli.discover_recovery_candidates", return_value=()), \
             patch("realmheart_installer.cli.InstallContext.create", return_value=context), \
             patch("realmheart_installer.cli.PreflightScanner") as scanner_cls, \
             patch("realmheart_installer.cli.InstallationPlanner") as planner_cls, \
             patch("realmheart_installer.cli.render_installation_plan", return_value="FULL PLAN"), \
             patch("realmheart_installer.cli._confirm", return_value=False), \
             patch("realmheart_installer.cli.PacmanAdapter") as pacman_cls, \
             patch("sys.stdout", output):
            scanner_cls.return_value.scan.return_value = snapshot
            planner_cls.return_value.build.return_value = plan
            self.assertEqual(main(["--install-dependencies", "install"]), 11)
        pacman_cls.assert_not_called()
        self.assertIn("FULL PLAN", output.getvalue())
        self.assertIn("before any package or Realmheart mutation", output.getvalue())
        self.assertEqual(context.transaction.state, TransactionState.FAILED)
        self.assertFalse(context.transaction.metadata["live_mutation_started"])


    def test_unexpected_startup_io_error_is_reported_without_traceback(self) -> None:
        stderr = io.StringIO()
        with patch("realmheart_installer.cli.ensure_not_root"), \
             patch("realmheart_installer.cli.XdgPaths.resolve", side_effect=OSError(5, "I/O failure")), \
             patch("sys.stderr", stderr):
            self.assertEqual(main(["preflight"]), 1)
        rendered = stderr.getvalue()
        self.assertIn("RH_UNEXPECTED_FAILURE", rendered)
        self.assertIn("OSError", rendered)
        self.assertNotIn("Traceback", rendered)

    def test_retired_legacy_installer_refuses_accidental_public_execution(self) -> None:
        script = _bootstrap.REPO_ROOT / "install-hypr-configs.sh"
        with tempfile.TemporaryDirectory(dir="/dev/shm") as temp:
            home = Path(temp) / "home"
            home.mkdir()
            env = os.environ.copy()
            env.pop("REALMHEART_ENABLE_LEGACY_INSTALLER", None)
            env["HOME"] = str(home)
            result = subprocess.run(
                [str(script)], cwd=_bootstrap.REPO_ROOT, env=env,
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5,
            )
            self.assertEqual(result.returncode, 64)
            self.assertIn("retired legacy installer", result.stderr)
            self.assertFalse((home / ".config/hypr").exists())


if __name__ == "__main__":
    unittest.main()
