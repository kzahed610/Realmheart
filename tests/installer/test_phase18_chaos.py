from __future__ import annotations

import errno
import io
import json
import os
import signal
import tempfile
import unittest
from argparse import Namespace
from dataclasses import replace
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

from . import _bootstrap  # noqa: F401

from realmheart_installer.context import InstallContext, XdgPaths
from realmheart_installer.errors import OperationExecutionError
from realmheart_installer.filesystem.compare import fingerprint_path
from realmheart_installer.filesystem.staging import FullTreeSwap, prepare_full_tree_stage
from realmheart_installer.finalization import FinalAction
from realmheart_installer.models import (
    MutationPrecondition,
    OperationSafety,
    OperationState,
    Reversibility,
    TransactionState,
    to_jsonable,
)
from realmheart_installer.package_manager.pacman import PacmanAdapter
from realmheart_installer.transaction.journal import WriteAheadJournal
from realmheart_installer.transaction.operations import (
    MovePathOperation,
    TransactionExecutor,
    WriteFileOperation,
)
from realmheart_installer.transaction.preconditions import FINGERPRINT_PRECONDITION
from realmheart_installer.transaction.recovery import (
    RecoveryStatus,
    acknowledge_manual_recovery,
    discover_recovery_candidates,
    inspect_journal,
    persist_recovery_report,
    recover_transaction_from_journal,
    rollback_transaction_from_journal,
)


class _PartialPacmanFailureRunner:
    """Small package-manager fake that installs one package, then fails."""

    def __init__(self) -> None:
        self.installed: dict[str, str] = {}
        self.repo = {"alpha": "1.0-1", "beta": "1.0-1"}

    def which(self, executable):
        return {
            "pacman": "/usr/bin/pacman",
            "sudo": "/usr/bin/sudo",
            "vercmp": "/usr/bin/vercmp",
        }.get(executable)

    def run(self, argv, **kwargs):
        from realmheart_installer.environment.command import CommandResult

        command = tuple(str(item) for item in argv)
        if command[:3] == ("/usr/bin/pacman", "-Q", "--"):
            package = command[3]
            if package in self.installed:
                return CommandResult(command, 0, f"{package} {self.installed[package]}\n")
            return CommandResult(command, 1, "", "not installed")
        if command[:3] == ("/usr/bin/pacman", "-Si", "--"):
            package = command[3]
            version = self.repo.get(package)
            if version:
                return CommandResult(command, 0, f"Repository : extra\nName : {package}\nVersion : {version}\n")
            return CommandResult(command, 1, "", "not found")
        if command == ("/usr/bin/pacman", "-Qu"):
            return CommandResult(command, 0, "")
        if command[:4] == ("/usr/bin/sudo", "/usr/bin/pacman", "-S", "--needed"):
            self.installed["alpha"] = self.repo["alpha"]
            return CommandResult(command, 1, "", "synthetic package transaction failure")
        return CommandResult(command, 1, "", "not mocked")


class Phase18ChaosTests(unittest.TestCase):
    def _tempdir(self):
        # /dev/shm is a normal Linux tmpfs and keeps fsync-heavy chaos tests fast
        # in constrained CI/sandbox environments.  Fall back to the platform
        # default if it is unavailable.
        base = "/dev/shm" if Path("/dev/shm").is_dir() else None
        return tempfile.TemporaryDirectory(dir=base)

    def _context(self, root: Path, txid: str = "RH-PHASE18") -> tuple[XdgPaths, InstallContext]:
        env = {
            "HOME": str(root / "home"),
            "XDG_CONFIG_HOME": str(root / "config"),
            "XDG_STATE_HOME": str(root / "state"),
            "XDG_DATA_HOME": str(root / "data"),
            "XDG_CACHE_HOME": str(root / "cache"),
            "XDG_RUNTIME_DIR": str(root / "run"),
        }
        paths = XdgPaths.resolve(env=env, uid=os.getuid())
        return paths, InstallContext.create(paths=paths, source_root=_bootstrap.REPO_ROOT, transaction_id=txid)

    def _swap_fixture(self, root: Path, txid: str):
        release = root / "release"
        active = root / "config" / "hypr"
        release.mkdir(parents=True)
        active.mkdir(parents=True)
        (release / "hyprland.conf").write_text("release=v2\n")
        (active / "hyprland.conf").write_text("user=v1\n")
        stage = prepare_full_tree_stage(
            release_tree=release,
            target=active,
            transaction_id=txid,
            preserve_relative_paths=(),
        )
        return active, stage

    def _assert_swap_recovers(self, root: Path, active: Path, stage) -> None:
        rollback_transaction_from_journal(root / "journal.jsonl", approved_roots=[root])
        self.assertEqual((active / "hyprland.conf").read_text(), "user=v1\n")
        self.assertEqual((stage.staging / "hyprland.conf").read_text(), "release=v2\n")
        self.assertFalse(stage.old.exists())
        self.assertTrue(inspect_journal(root / "journal.jsonl").is_clean)

    def test_swap_crash_before_first_rename_is_resolved_as_noop_rollback(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            active, stage = self._swap_fixture(root, "RH-CHAOS-A")
            journal = WriteAheadJournal(root / "journal.jsonl")
            original = MovePathOperation.apply
            calls = 0

            def crash_before_apply(operation):
                nonlocal calls
                calls += 1
                if calls == 1:
                    raise SystemExit(91)
                return original(operation)

            with patch.object(MovePathOperation, "apply", new=crash_before_apply):
                with self.assertRaises(SystemExit):
                    FullTreeSwap(stage, journal).execute()
            self.assertTrue(active.exists())
            self._assert_swap_recovers(root, active, stage)

    def test_swap_crash_after_first_rename_before_completed_recovers(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            active, stage = self._swap_fixture(root, "RH-CHAOS-B")
            journal = WriteAheadJournal(root / "journal.jsonl")
            original = journal.append
            completed = 0

            def crash_on_first_completed(**kwargs):
                nonlocal completed
                if kwargs.get("state") is OperationState.COMPLETED:
                    completed += 1
                    if completed == 1:
                        raise SystemExit(92)
                return original(**kwargs)

            with patch.object(journal, "append", side_effect=crash_on_first_completed):
                with self.assertRaises(SystemExit):
                    FullTreeSwap(stage, journal).execute()
            self.assertFalse(active.exists())
            self.assertTrue(stage.old.exists())
            self._assert_swap_recovers(root, active, stage)

    def test_swap_crash_before_second_rename_recovers(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            active, stage = self._swap_fixture(root, "RH-CHAOS-C")
            journal = WriteAheadJournal(root / "journal.jsonl")
            original = MovePathOperation.apply
            calls = 0

            def crash_before_second(operation):
                nonlocal calls
                calls += 1
                if calls == 2:
                    raise SystemExit(93)
                return original(operation)

            with patch.object(MovePathOperation, "apply", new=crash_before_second):
                with self.assertRaises(SystemExit):
                    FullTreeSwap(stage, journal).execute()
            self.assertFalse(active.exists())
            self.assertTrue(stage.old.exists())
            self.assertTrue(stage.staging.exists())
            self._assert_swap_recovers(root, active, stage)

    def test_swap_crash_after_second_rename_before_completed_recovers(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            active, stage = self._swap_fixture(root, "RH-CHAOS-D")
            journal = WriteAheadJournal(root / "journal.jsonl")
            original = journal.append
            completed = 0

            def crash_on_second_completed(**kwargs):
                nonlocal completed
                if kwargs.get("state") is OperationState.COMPLETED:
                    completed += 1
                    if completed == 2:
                        raise SystemExit(94)
                return original(**kwargs)

            with patch.object(journal, "append", side_effect=crash_on_second_completed):
                with self.assertRaises(SystemExit):
                    FullTreeSwap(stage, journal).execute()
            self.assertEqual((active / "hyprland.conf").read_text(), "release=v2\n")
            self.assertTrue(stage.old.exists())
            self._assert_swap_recovers(root, active, stage)

    def test_journal_failure_before_mutation_leaves_target_untouched_and_recoverable(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            paths, context = self._context(root, "RH-JOURNAL-BEFORE")
            target = paths.config_home / "example.conf"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("old\n")
            preimage = context.preimage_dir / "example.preimage"
            op = WriteFileOperation(
                target=target,
                allowed_root=paths.config_home,
                safety=OperationSafety(
                    Reversibility.EXACT,
                    MutationPrecondition(FINGERPRINT_PRECONDITION, fingerprint_path(target), True),
                    str(preimage),
                ),
                content=b"new\n",
                mode=0o600,
                preimage_path=preimage,
            )
            journal = WriteAheadJournal(context.journal_path)
            original = journal.append

            def fail_started(**kwargs):
                if kwargs.get("state") is OperationState.STARTED:
                    raise OSError(errno.EIO, "synthetic journal failure")
                return original(**kwargs)

            with patch.object(journal, "append", side_effect=fail_started):
                with self.assertRaises(OSError):
                    TransactionExecutor(journal).execute(op)
            self.assertEqual(target.read_text(), "old\n")
            rollback_transaction_from_journal(context.journal_path, approved_roots=[paths.config_home, context.transaction_dir])
            self.assertEqual(target.read_text(), "old\n")
            self.assertTrue(inspect_journal(context.journal_path).is_clean)

    def test_journal_completion_failure_after_mutation_is_detected_and_reversed(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            paths, context = self._context(root, "RH-JOURNAL-AFTER")
            target = paths.config_home / "example.conf"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("old\n")
            preimage = context.preimage_dir / "example.preimage"
            op = WriteFileOperation(
                target=target,
                allowed_root=paths.config_home,
                safety=OperationSafety(
                    Reversibility.EXACT,
                    MutationPrecondition(FINGERPRINT_PRECONDITION, fingerprint_path(target), True),
                    str(preimage),
                ),
                content=b"new\n",
                mode=0o600,
                preimage_path=preimage,
            )
            journal = WriteAheadJournal(context.journal_path)
            original = journal.append

            def fail_completed(**kwargs):
                if kwargs.get("state") is OperationState.COMPLETED:
                    raise OSError(errno.EIO, "synthetic completion journal failure")
                return original(**kwargs)

            with patch.object(journal, "append", side_effect=fail_completed):
                with self.assertRaises(OSError):
                    TransactionExecutor(journal).execute(op)
            self.assertEqual(target.read_text(), "new\n")
            rollback_transaction_from_journal(context.journal_path, approved_roots=[paths.config_home, context.transaction_dir])
            self.assertEqual(target.read_text(), "old\n")
            self.assertTrue(inspect_journal(context.journal_path).is_clean)

    def test_disk_full_recovery_report_sacrifices_reserved_space_and_retries(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            _paths, context = self._context(root, "RH-DISK-FULL")
            self.assertFalse(context.recovery_reserve_path.exists())
            context.ensure_recovery_reserve()
            self.assertTrue(context.recovery_reserve_path.is_file())
            self.assertGreaterEqual(context.recovery_reserve_path.stat().st_size, 8 * 1024)
            context.transaction.transition(TransactionState.FAILED)
            original = context.persist_json
            failed = False

            def one_enospc(filename, payload):
                nonlocal failed
                if filename == "recovery.json" and not failed:
                    failed = True
                    raise OSError(errno.ENOSPC, "synthetic disk full")
                return original(filename, payload)

            with patch.object(context, "persist_json", side_effect=one_enospc):
                report = persist_recovery_report(context, trigger="disk_full", primary_error="ENOSPC")
            self.assertTrue(failed)
            self.assertFalse(context.recovery_reserve_path.exists())
            self.assertTrue((context.transaction_dir / "recovery.json").is_file())
            self.assertEqual(report.status, RecoveryStatus.CLEAN)

    def test_missing_write_preimage_makes_recovery_manual_attention(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            paths, context = self._context(root, "RH-MISSING-PREIMAGE")
            target = paths.config_home / "important.conf"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("original\n")
            preimage = context.preimage_dir / "important.preimage"
            op = WriteFileOperation(
                target=target,
                allowed_root=paths.config_home,
                safety=OperationSafety(
                    Reversibility.EXACT,
                    MutationPrecondition(FINGERPRINT_PRECONDITION, fingerprint_path(target), True),
                    str(preimage),
                ),
                content=b"realmheart\n",
                mode=0o600,
                preimage_path=preimage,
            )
            TransactionExecutor(WriteAheadJournal(context.journal_path)).execute(op)
            preimage.unlink()
            context.transaction.transition(TransactionState.INTERRUPTED)
            context.transaction.metadata["live_mutation_started"] = True
            context.persist_summary()
            with self.assertRaises(OperationExecutionError):
                rollback_transaction_from_journal(
                    context.journal_path,
                    approved_roots=[paths.config_home, context.transaction_dir],
                )
            report = persist_recovery_report(context, trigger="missing_backup_file")
            self.assertEqual(report.status, RecoveryStatus.MANUAL_ATTENTION)
            self.assertFalse(report.automatic_rollback_safe)
            self.assertEqual(report.operations[0].disposition, "ambiguous")



    def test_standalone_dependency_reprobe_failure_writes_recovery_report_and_success_releases_reserve(self) -> None:
        from tests.installer.test_preflight import FakeRunner, PreflightTests, capability
        from realmheart_installer import cli as cli_module
        from realmheart_installer.environment.capabilities import CapabilityState
        from realmheart_installer.environment.preflight import PreflightState
        from realmheart_installer.package_manager.base import (
            DependencyPackagePlan, PackageInstallResult, PackageProvenance,
        )

        class SuccessfulAdapter:
            def __init__(self, _runner):
                pass

            def install(self, packages, *, required_by=None):
                requested = tuple(packages)
                provenance = tuple(
                    PackageProvenance(
                        package=item, required_by=(), installed_before=False, version_before=None,
                        repository="extra", repository_version="1.0-1", install_attempted=True,
                        installed_by_transaction=True, changed_by_transaction=True,
                        version_after="1.0-1", result="pass",
                    )
                    for item in requested
                )
                return PackageInstallResult("pacman", requested, ("sudo", "pacman"), 0, provenance)

        with self._tempdir() as temp:
            root = Path(temp)
            preflight_fixture = PreflightTests(methodName="test_missing_required_capability_blocks")
            snapshot = preflight_fixture._scan(
                root, FakeRunner(package_manager="pacman"),
                (capability("runtime.grim", state=CapabilityState.MISSING),),
            )
            plan = DependencyPackagePlan("pacman", (), ("grim",), ())
            args = Namespace(
                json=False, install_dependencies=True, dry_run=False, yes=True,
            )
            runner = FakeRunner(package_manager="pacman")

            # Package mutation succeeds, but direct capability verification still
            # fails.  Because an external package action already happened, this
            # must leave a durable manual-attention recovery report.
            paths, context = self._context(root / "reprobe-fail", "RH-DEPS-REPROBE-FAIL")
            with patch.object(cli_module, "PacmanAdapter", SuccessfulAdapter), patch.object(
                cli_module, "build_pacman_dependency_plan", return_value=plan
            ), patch.object(cli_module, "package_required_by", return_value={}), patch.object(
                cli_module.PreflightScanner, "scan", return_value=snapshot
            ), redirect_stdout(io.StringIO()):
                code = cli_module._run_dependencies(args, paths, context, snapshot, runner, context.transaction.transaction_id)
            self.assertEqual(code, 1)
            self.assertEqual(context.transaction.state, TransactionState.FAILED)
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["trigger"], "dependency_reprobe_failure")
            self.assertEqual(payload["status"], "manual_attention")
            self.assertTrue(context.recovery_reserve_path.exists())

            # The same package transaction followed by a successful re-probe is a
            # committed standalone dependency transaction and must not leak the
            # emergency reserve into historical transaction state.
            verified = replace(
                snapshot,
                capabilities=(capability("runtime.grim", state=CapabilityState.PASS),),
                state=PreflightState.READY, blockers=(),
            )
            paths2, context2 = self._context(root / "reprobe-pass", "RH-DEPS-REPROBE-PASS")
            with patch.object(cli_module, "PacmanAdapter", SuccessfulAdapter), patch.object(
                cli_module, "build_pacman_dependency_plan", return_value=plan
            ), patch.object(cli_module, "package_required_by", return_value={}), patch.object(
                cli_module.PreflightScanner, "scan", return_value=verified
            ), redirect_stdout(io.StringIO()):
                code = cli_module._run_dependencies(args, paths2, context2, snapshot, runner, context2.transaction.transaction_id)
            self.assertEqual(code, 0)
            self.assertEqual(context2.transaction.state, TransactionState.COMMITTED)
            self.assertFalse(context2.recovery_reserve_path.exists())


    def test_hard_death_during_package_transaction_is_manual_attention_and_blocks_reentry(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            paths, context = self._context(root, "RH-PACKAGE-HARD-DEATH")
            context.ensure_recovery_reserve()
            context.transaction.transition(TransactionState.APPLYING)
            context.transaction.metadata["package_install_started"] = True
            context.persist_summary()

            # Simulate process death while the external package manager owns the
            # mutation boundary: no PackageInstallResult can be persisted yet and
            # there is intentionally no filesystem WAL entry to reverse.
            candidates = discover_recovery_candidates(paths, persist_reports=True)
            self.assertEqual(len(candidates), 1)
            candidate = candidates[0]
            self.assertEqual(candidate.status, RecoveryStatus.MANUAL_ATTENTION)
            self.assertTrue(candidate.blocks_new_transaction)
            self.assertFalse(candidate.automatic_rollback_safe)
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["status"], "manual_attention")
            self.assertIn("automatic recovery is not proven safe", payload["note"])
            self.assertTrue(context.recovery_reserve_path.exists())


    def test_partial_dependency_install_is_reported_as_best_effort_manual_attention(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            _paths, context = self._context(root, "RH-PACKAGE-FAIL")
            runner = _PartialPacmanFailureRunner()
            result = PacmanAdapter(runner).install(("alpha", "beta"))
            self.assertFalse(result.ok)
            self.assertTrue(result.provenance[0].installed_by_transaction)
            self.assertFalse(result.provenance[1].installed_by_transaction)
            context.transaction.metadata["package_install"] = to_jsonable(result)
            context.transaction.transition(TransactionState.FAILED)
            context.persist_summary()
            report = persist_recovery_report(
                context, trigger="dependency_install_failure", primary_error=result.error,
            )
            self.assertEqual(report.status, RecoveryStatus.MANUAL_ATTENTION)
            self.assertFalse(report.automatic_rollback_safe)
            candidates = discover_recovery_candidates(context.paths)
            self.assertEqual(len(candidates), 1)
            self.assertFalse(candidates[0].blocks_new_transaction)

    def test_manual_attention_can_be_explicitly_acknowledged_without_claiming_rollback(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            _paths, context = self._context(root, "RH-MANUAL-ACK")
            context.transaction.transition(TransactionState.INTERRUPTED)
            context.transaction.metadata["live_mutation_started"] = True
            context.persist_summary()
            journal = WriteAheadJournal(context.journal_path)
            journal.append(
                operation_id="custom-service-op", state=OperationState.INTENT,
                kind="systemd_user_services", target="realmheart.service", data={},
            )
            journal.append(
                operation_id="custom-service-op", state=OperationState.STARTED,
                kind="systemd_user_services", target="realmheart.service",
            )
            journal.append(
                operation_id="custom-service-op", state=OperationState.COMPLETED,
                kind="systemd_user_services", target="realmheart.service",
            )
            before = persist_recovery_report(context, trigger="synthetic_custom_interrupt")
            self.assertEqual(before.status, RecoveryStatus.MANUAL_ATTENTION)
            candidate = discover_recovery_candidates(context.paths)[0]
            self.assertTrue(candidate.blocks_new_transaction)

            after = acknowledge_manual_recovery(context)
            self.assertEqual(after.status, RecoveryStatus.CLEAN)
            self.assertEqual(context.transaction.state, TransactionState.FAILED)
            self.assertTrue(context.transaction.metadata["manual_recovery_acknowledged"])
            self.assertIn("explicitly acknowledged", after.note)
            self.assertEqual(discover_recovery_candidates(context.paths), ())

    def test_live_keyboard_interrupt_marks_transaction_interrupted_and_emits_report(self) -> None:
        from tests.installer.test_live_install import Phase16LiveInstallTests
        from realmheart_installer.live import orchestrator as live_module
        from realmheart_installer.live.orchestrator import LiveInstallExecutor

        with self._tempdir() as temp:
            root = Path(temp)
            fixture = Phase16LiveInstallTests(methodName="test_full_fake_root_healthy_keep_commits_receipt_and_preserves_user_state")
            paths, snapshot, registry, plan, build, runner, context = fixture._fixture(root)
            marker = paths.config_home / "phase18-interrupt.marker"

            def mutate_then_interrupt(*args, **kwargs):
                marker.parent.mkdir(parents=True, exist_ok=True)
                op = WriteFileOperation(
                    target=marker,
                    allowed_root=paths.config_home,
                    safety=OperationSafety(
                        Reversibility.EXACT,
                        MutationPrecondition(FINGERPRINT_PRECONDITION, fingerprint_path(marker), False),
                        str(context.preimage_dir / "interrupt-marker.preimage"),
                    ),
                    content=b"partial\n",
                    mode=0o600,
                    preimage_path=context.preimage_dir / "interrupt-marker.preimage",
                )
                TransactionExecutor(WriteAheadJournal(context.journal_path)).execute(op)
                raise KeyboardInterrupt()

            with patch.object(live_module, "NativeBuildExecutor") as build_cls, patch.object(
                live_module, "execute_installation_components", side_effect=mutate_then_interrupt
            ):
                build_cls.return_value.run.return_value = build
                with self.assertRaises(KeyboardInterrupt):
                    LiveInstallExecutor(
                        plan=plan, registry=registry, context=context, paths=paths,
                        source_root=_bootstrap.REPO_ROOT, runner=runner, snapshot=snapshot,
                        decision_selector=lambda decision: FinalAction.KEEP,
                        package_actions_applied=True, allow_unprivileged_system_commit=True,
                        privileged_uid=os.getuid(), privileged_gid=os.getgid(),
                        persist_diagnostic_on_failure=False,
                    ).run()
            self.assertEqual(context.transaction.state, TransactionState.INTERRUPTED)
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["trigger"], "process_interruption")
            self.assertEqual(payload["status"], "recovery_available")
            self.assertTrue(marker.exists())
            rollback_transaction_from_journal(
                context.journal_path,
                approved_roots=[paths.config_home, context.transaction_dir],
            )
            self.assertFalse(marker.exists())



    def test_live_sigterm_uses_controlled_interruption_path_and_records_signal(self) -> None:
        from tests.installer.test_live_install import Phase16LiveInstallTests
        from realmheart_installer import cli as cli_module
        from realmheart_installer.errors import TerminationSignalInterrupt
        from realmheart_installer.live import orchestrator as live_module
        from realmheart_installer.live.orchestrator import LiveInstallExecutor

        with self._tempdir() as temp:
            root = Path(temp)
            fixture = Phase16LiveInstallTests(methodName="test_full_fake_root_healthy_keep_commits_receipt_and_preserves_user_state")
            paths, snapshot, registry, plan, build, runner, context = fixture._fixture(root)
            marker = paths.config_home / "phase18-sigterm.marker"

            def mutate_then_sigterm(*args, **kwargs):
                marker.parent.mkdir(parents=True, exist_ok=True)
                op = WriteFileOperation(
                    target=marker,
                    allowed_root=paths.config_home,
                    safety=OperationSafety(
                        Reversibility.EXACT,
                        MutationPrecondition(FINGERPRINT_PRECONDITION, fingerprint_path(marker), False),
                        str(context.preimage_dir / "sigterm-marker.preimage"),
                    ),
                    content=b"partial\n",
                    mode=0o600,
                    preimage_path=context.preimage_dir / "sigterm-marker.preimage",
                )
                TransactionExecutor(WriteAheadJournal(context.journal_path)).execute(op)
                signal.raise_signal(signal.SIGTERM)

            previous = cli_module._install_termination_signal_handler()
            try:
                with patch.object(live_module, "NativeBuildExecutor") as build_cls, patch.object(
                    live_module, "execute_installation_components", side_effect=mutate_then_sigterm
                ):
                    build_cls.return_value.run.return_value = build
                    with self.assertRaises(TerminationSignalInterrupt):
                        LiveInstallExecutor(
                            plan=plan, registry=registry, context=context, paths=paths,
                            source_root=_bootstrap.REPO_ROOT, runner=runner, snapshot=snapshot,
                            decision_selector=lambda decision: FinalAction.KEEP,
                            package_actions_applied=True, allow_unprivileged_system_commit=True,
                            privileged_uid=os.getuid(), privileged_gid=os.getgid(),
                            persist_diagnostic_on_failure=False,
                        ).run()
            finally:
                cli_module._restore_termination_signal_handler(previous)

            self.assertEqual(context.transaction.state, TransactionState.INTERRUPTED)
            self.assertEqual(context.transaction.metadata["interruption"], "sigterm")
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["trigger"], "process_interruption")
            self.assertEqual(payload["status"], "recovery_available")
            self.assertIn("TerminationSignalInterrupt", payload["primary_error"])
            self.assertTrue(marker.exists())
            rollback_transaction_from_journal(
                context.journal_path,
                approved_roots=[paths.config_home, context.transaction_dir],
            )
            self.assertFalse(marker.exists())



    def test_interruption_after_live_commit_never_downgrades_transaction_to_interrupted(self) -> None:
        from tests.installer.test_live_install import Phase16LiveInstallTests
        from realmheart_installer.live import orchestrator as live_module
        from realmheart_installer.live.orchestrator import LiveInstallExecutor

        with self._tempdir() as temp:
            root = Path(temp)
            fixture = Phase16LiveInstallTests(methodName="test_full_fake_root_healthy_keep_commits_receipt_and_preserves_user_state")
            paths, snapshot, registry, plan, build, runner, context = fixture._fixture(root)
            original_persist_json = context.persist_json

            def interrupt_after_commit(filename, payload):
                if filename == "finalization.json":
                    raise KeyboardInterrupt()
                return original_persist_json(filename, payload)

            with patch.object(live_module, "NativeBuildExecutor") as build_cls, patch.object(
                context, "persist_json", side_effect=interrupt_after_commit
            ):
                build_cls.return_value.run.return_value = build
                with self.assertRaises(KeyboardInterrupt):
                    LiveInstallExecutor(
                        plan=plan, registry=registry, context=context, paths=paths,
                        source_root=_bootstrap.REPO_ROOT, runner=runner, snapshot=snapshot,
                        decision_selector=lambda decision: FinalAction.KEEP,
                        package_actions_applied=True, allow_unprivileged_system_commit=True,
                        privileged_uid=os.getuid(), privileged_gid=os.getgid(),
                        persist_diagnostic_on_failure=False,
                    ).run()

            self.assertEqual(context.transaction.state, TransactionState.COMMITTED)
            self.assertEqual(context.transaction.metadata["interruption_after_terminal"], "keyboard_interrupt")
            self.assertTrue((paths.realmheart_state / "installed-state.json").exists())
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["status"], "clean")
            self.assertEqual(discover_recovery_candidates(paths), ())

    def test_interruption_after_uninstall_commit_preserves_committed_uninstall(self) -> None:
        from tests.installer.test_uninstall import NoSystemRunner, Phase17UninstallTests
        from realmheart_installer.uninstall import UninstallConfigAction, UninstallExecutor

        with self._tempdir() as temp:
            root = Path(temp)
            fixture_builder = Phase17UninstallTests(methodName="test_keep_current_removes_realmheart_integration_but_preserves_user_config_and_history")
            fixture = fixture_builder._fixture(root)
            plan = fixture_builder._plan(fixture, txid="RH-PHASE18-UNINSTALL-TERMINAL")
            context = InstallContext.create(
                paths=fixture["paths"], source_root=_bootstrap.REPO_ROOT, transaction_id=plan.transaction_id,
            )
            original_persist_json = context.persist_json

            def interrupt_result_write(filename, payload):
                if filename == "uninstall-result.json":
                    raise KeyboardInterrupt()
                return original_persist_json(filename, payload)

            with patch.object(context, "persist_json", side_effect=interrupt_result_write):
                with self.assertRaises(KeyboardInterrupt):
                    UninstallExecutor(
                        plan=plan, context=context, paths=fixture["paths"], runner=NoSystemRunner(),
                        config_action=UninstallConfigAction.KEEP_CURRENT,
                    ).run()

            self.assertEqual(context.transaction.state, TransactionState.COMMITTED)
            self.assertEqual(context.transaction.metadata["interruption_after_terminal"], "keyboard_interrupt")
            self.assertFalse(fixture["receipt"].exists())
            self.assertFalse(fixture["dropin"].exists())
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["status"], "clean")
            candidates = discover_recovery_candidates(fixture["paths"])
            self.assertFalse(any(item.transaction_id == plan.transaction_id for item in candidates))


    def test_hard_process_death_is_reconstructed_on_next_invocation_and_safely_recovered(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            paths, context = self._context(root, "RH-HARD-CRASH")
            context.transaction.transition(TransactionState.APPLYING)
            context.transaction.metadata["live_mutation_started"] = True
            context.persist_summary()

            target = paths.config_home / "crash-marker.conf"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("before\n")
            preimage = context.preimage_dir / "crash-marker.preimage"
            op = WriteFileOperation(
                target=target,
                allowed_root=paths.config_home,
                safety=OperationSafety(
                    Reversibility.EXACT,
                    MutationPrecondition(FINGERPRINT_PRECONDITION, fingerprint_path(target), True),
                    str(preimage),
                ),
                content=b"after\n", mode=0o600, preimage_path=preimage,
            )
            journal = WriteAheadJournal(context.journal_path)
            original = journal.append

            def die_before_completed(**kwargs):
                if kwargs.get("state") is OperationState.COMPLETED:
                    raise SystemExit(137)
                return original(**kwargs)

            with patch.object(journal, "append", side_effect=die_before_completed):
                with self.assertRaises(SystemExit):
                    TransactionExecutor(journal).execute(op)
            self.assertEqual(target.read_text(), "after\n")
            self.assertFalse((context.transaction_dir / "recovery.json").exists())

            # Simulate a fresh process: discovery reloads transaction.json and
            # journal.jsonl instead of relying on any in-memory object.
            candidates = discover_recovery_candidates(paths, persist_reports=True)
            self.assertEqual(len(candidates), 1)
            self.assertEqual(candidates[0].transaction_id, "RH-HARD-CRASH")
            self.assertEqual(candidates[0].status, RecoveryStatus.RECOVERY_AVAILABLE)
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["trigger"], "startup_reentry")
            self.assertTrue(payload["automatic_rollback_safe"])

            loaded = InstallContext.load_existing(paths=paths, transaction_id="RH-HARD-CRASH")
            report = recover_transaction_from_journal(loaded)
            self.assertEqual(report.status, RecoveryStatus.CLEAN)
            self.assertEqual(loaded.transaction.state, TransactionState.ROLLED_BACK)
            self.assertEqual(target.read_text(), "before\n")
            self.assertEqual(discover_recovery_candidates(paths), ())

    def test_tampered_wal_path_is_rejected_before_filesystem_fingerprinting(self) -> None:
        from realmheart_installer.transaction import recovery as recovery_module

        with self._tempdir() as temp:
            root = Path(temp)
            _paths, context = self._context(root, "RH-TAMPERED-WAL")
            context.transaction.transition(TransactionState.INTERRUPTED)
            context.transaction.metadata["live_mutation_started"] = True
            context.persist_summary()
            journal = WriteAheadJournal(context.journal_path)
            journal.append(
                operation_id="evil", state=OperationState.INTENT, kind="write_file",
                target="/etc/shadow",
                data={
                    "allowed_root": "/etc",
                    "before_fingerprint": "attacker-controlled",
                    "expected_after_fingerprint": "attacker-controlled",
                    "existed_before": False,
                },
            )
            journal.append(
                operation_id="evil", state=OperationState.STARTED, kind="write_file", target="/etc/shadow",
            )
            with patch.object(recovery_module, "fingerprint_path", side_effect=AssertionError("must not inspect untrusted path")):
                report = persist_recovery_report(context, trigger="tampered_wal")
            self.assertEqual(report.status, RecoveryStatus.MANUAL_ATTENTION)
            self.assertEqual(report.operations[0].disposition, "ambiguous")
            self.assertFalse(report.operations[0].automatically_reversible)


    def test_terminal_summary_cannot_hide_incomplete_wal_operation(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            paths, context = self._context(root, "RH-TERMINAL-WAL-MISMATCH")
            target = paths.config_home / "terminal-mismatch.conf"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("before\n")
            journal = WriteAheadJournal(context.journal_path)
            journal.append(
                operation_id="op-terminal-mismatch",
                state=OperationState.INTENT,
                kind="write_file",
                target=str(target),
                data={
                    "allowed_root": str(paths.config_home),
                    "before_fingerprint": fingerprint_path(target),
                    "expected_after_fingerprint": "sha256:synthetic-after",
                    "existed_before": True,
                    "preimage_path": str(context.preimage_dir / "terminal-mismatch.preimage"),
                },
            )
            journal.append(
                operation_id="op-terminal-mismatch",
                state=OperationState.STARTED,
                kind="write_file",
                target=str(target),
            )
            context.transaction.transition(TransactionState.COMMITTED)
            context.persist_summary()

            report = persist_recovery_report(context, trigger="terminal_wal_mismatch")
            self.assertEqual(report.status, RecoveryStatus.MANUAL_ATTENTION)
            self.assertEqual(report.incomplete_operation_count, 1)
            self.assertFalse(report.automatic_rollback_safe)
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["status"], "manual_attention")


    def test_malformed_abandoned_transaction_gets_manual_attention_report(self) -> None:
        with self._tempdir() as temp:
            root = Path(temp)
            paths, context = self._context(root, "RH-CORRUPT-REENTRY")
            context.transaction.transition(TransactionState.APPLYING)
            context.persist_summary()
            (context.transaction_dir / "transaction.json").write_text("{ definitely not json\n")

            candidates = discover_recovery_candidates(paths, persist_reports=True)
            self.assertEqual(len(candidates), 1)
            candidate = candidates[0]
            self.assertEqual(candidate.status, RecoveryStatus.MANUAL_ATTENTION)
            self.assertFalse(candidate.automatic_rollback_safe)
            self.assertIsNotNone(candidate.load_error)
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["status"], "manual_attention")
            self.assertEqual(payload["transaction_state"], "unknown")


    def test_public_recovery_cli_lists_inspects_and_rolls_back_abandoned_transaction(self) -> None:
        from realmheart_installer import cli as cli_module

        with self._tempdir() as temp:
            root = Path(temp)
            paths, context = self._context(root, "RH-RECOVERY-CLI")
            context.transaction.transition(TransactionState.APPLYING)
            context.transaction.metadata["live_mutation_started"] = True
            context.persist_summary()

            target = paths.config_home / "recovery-cli.conf"
            target.parent.mkdir(parents=True, exist_ok=True)
            op = WriteFileOperation(
                target=target,
                allowed_root=paths.config_home,
                safety=OperationSafety(
                    Reversibility.EXACT,
                    MutationPrecondition(FINGERPRINT_PRECONDITION, fingerprint_path(target), False),
                    str(context.preimage_dir / "recovery-cli.preimage"),
                ),
                content=b"partial\n",
                mode=0o600,
                preimage_path=context.preimage_dir / "recovery-cli.preimage",
            )
            TransactionExecutor(WriteAheadJournal(context.journal_path)).execute(op)
            self.assertTrue(target.exists())

            def run_cli(argv):
                stdout = io.StringIO()
                stderr = io.StringIO()
                with patch.object(cli_module, "ensure_not_root"), patch.object(
                    cli_module.XdgPaths, "resolve", return_value=paths
                ), redirect_stdout(stdout), redirect_stderr(stderr):
                    code = cli_module.main(argv)
                return code, stdout.getvalue(), stderr.getvalue()

            code, output, error = run_cli(["--json", "recovery-list"])
            self.assertEqual(code, 23, error)
            listing = json.loads(output)
            self.assertEqual(listing["recovery_candidates"][0]["transaction_id"], context.transaction.transaction_id)
            self.assertEqual(listing["recovery_candidates"][0]["status"], "recovery_available")

            code, output, error = run_cli([
                "--json", "--transaction-id", context.transaction.transaction_id, "recovery-inspect"
            ])
            self.assertEqual(code, 23, error)
            inspection = json.loads(output)
            self.assertEqual(inspection["status"], "recovery_available")
            self.assertTrue(inspection["automatic_rollback_safe"])

            code, output, error = run_cli([
                "--json", "--transaction-id", context.transaction.transaction_id, "recovery-rollback"
            ])
            self.assertEqual(code, 0, error)
            recovered = json.loads(output)
            self.assertEqual(recovered["status"], "clean")
            self.assertEqual(recovered["transaction_state"], "rolled_back")
            self.assertFalse(target.exists())

            reloaded = InstallContext.load_existing(paths=paths, transaction_id=context.transaction.transaction_id)
            self.assertEqual(reloaded.transaction.state, TransactionState.ROLLED_BACK)
            self.assertEqual(discover_recovery_candidates(paths), ())


    def test_new_mutating_cli_transaction_is_blocked_by_abandoned_recoverable_transaction(self) -> None:
        from realmheart_installer import cli as cli_module

        with self._tempdir() as temp:
            root = Path(temp)
            paths, context = self._context(root, "RH-BLOCK-NEXT")
            context.transaction.transition(TransactionState.APPLYING)
            context.transaction.metadata["live_mutation_started"] = True
            context.persist_summary()
            target = paths.config_home / "owned.conf"
            target.parent.mkdir(parents=True, exist_ok=True)
            op = WriteFileOperation(
                target=target, allowed_root=paths.config_home,
                safety=OperationSafety(
                    Reversibility.EXACT,
                    MutationPrecondition(FINGERPRINT_PRECONDITION, fingerprint_path(target), False),
                    str(context.preimage_dir / "unused.preimage"),
                ),
                content=b"partial\n", mode=0o600,
                preimage_path=context.preimage_dir / "unused.preimage",
            )
            TransactionExecutor(WriteAheadJournal(context.journal_path)).execute(op)

            before = {item.name for item in paths.transactions.iterdir()}
            with patch.object(cli_module, "ensure_not_root"), patch.object(
                cli_module.XdgPaths, "resolve", return_value=paths
            ), redirect_stderr(io.StringIO()):
                exit_code = cli_module.main(["install"])
            after = {item.name for item in paths.transactions.iterdir()}
            self.assertEqual(exit_code, 2)
            self.assertEqual(before, after)
            self.assertTrue((context.transaction_dir / "recovery.json").exists())

    def test_verification_failure_rollback_emits_clean_recovery_report(self) -> None:
        from tests.installer.test_live_install import Phase16LiveInstallTests
        from realmheart_installer.live import orchestrator as live_module
        from realmheart_installer.live.orchestrator import LiveInstallExecutor

        with self._tempdir() as temp:
            root = Path(temp)
            fixture = Phase16LiveInstallTests(methodName="test_full_fake_root_failed_essential_probe_rolls_back_exactly_and_keeps_no_receipt")
            paths, snapshot, registry, plan, build, runner, context = fixture._fixture(root, fail_event_ping=True)
            with patch.object(live_module, "NativeBuildExecutor") as build_cls:
                build_cls.return_value.run.return_value = build
                result = LiveInstallExecutor(
                    plan=plan, registry=registry, context=context, paths=paths,
                    source_root=_bootstrap.REPO_ROOT, runner=runner, snapshot=snapshot,
                    decision_selector=lambda decision: FinalAction.RESTORE_PREVIOUS,
                    package_actions_applied=True, allow_unprivileged_system_commit=True,
                    privileged_uid=os.getuid(), privileged_gid=os.getgid(),
                    persist_diagnostic_on_failure=False,
                ).run()
            self.assertEqual(result.finalization.exit_code, 21)
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["trigger"], "finalization_rollback")
            self.assertEqual(payload["status"], "clean")

    def test_active_tree_mutation_between_plan_and_takeover_is_preserved_and_reported(self) -> None:
        from tests.installer.test_live_install import Phase16LiveInstallTests
        from realmheart_installer.live import orchestrator as live_module
        from realmheart_installer.live.orchestrator import LiveInstallExecutor

        with self._tempdir() as temp:
            root = Path(temp)
            fixture = Phase16LiveInstallTests(methodName="test_full_fake_root_healthy_keep_commits_receipt_and_preserves_user_state")
            paths, snapshot, registry, plan, build, runner, context = fixture._fixture(root)
            external = paths.config_home / "hypr" / "edited-after-plan.conf"
            external.write_text("USER CHANGED THIS AFTER PLAN\n")
            with patch.object(live_module, "NativeBuildExecutor") as build_cls:
                build_cls.return_value.run.return_value = build
                result = LiveInstallExecutor(
                    plan=plan, registry=registry, context=context, paths=paths,
                    source_root=_bootstrap.REPO_ROOT, runner=runner, snapshot=snapshot,
                    decision_selector=lambda decision: FinalAction.RESTORE_PREVIOUS,
                    package_actions_applied=True, allow_unprivileged_system_commit=True,
                    privileged_uid=os.getuid(), privileged_gid=os.getgid(),
                    persist_diagnostic_on_failure=False,
                ).run()
            hypr = next(item for item in result.component_report.results if item.component_id == "hypr-integration")
            self.assertEqual(hypr.state.value, "failed")
            self.assertEqual(hypr.error_code, "RH_PRECONDITION_DRIFT")
            self.assertIn("target changed since InstallationPlan approval", hypr.reason or "")
            self.assertEqual(external.read_text(), "USER CHANGED THIS AFTER PLAN\n")
            self.assertEqual(context.transaction.state, TransactionState.ROLLED_BACK)
            self.assertEqual(result.finalization.exit_code, 21)
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["trigger"], "finalization_rollback")
            self.assertEqual(payload["status"], "clean")

    def test_receipt_completion_write_interruption_restores_previous_state_and_reports(self) -> None:
        from tests.installer.test_live_install import Phase16LiveInstallTests
        from realmheart_installer.finalization import receipt as receipt_module
        from realmheart_installer.live import orchestrator as live_module
        from realmheart_installer.live.orchestrator import LiveInstallExecutor

        with self._tempdir() as temp:
            root = Path(temp)
            fixture = Phase16LiveInstallTests(methodName="test_full_fake_root_healthy_keep_commits_receipt_and_preserves_user_state")
            paths, snapshot, registry, plan, build, runner, context = fixture._fixture(root)
            receipt_path = paths.realmheart_state / "installed-state.json"
            original_append = receipt_module.WriteAheadJournal.append

            def fail_receipt_completed(journal, **kwargs):
                if kwargs.get("state") is OperationState.COMPLETED and kwargs.get("target") == str(receipt_path):
                    raise OSError(errno.EIO, "synthetic receipt completion interruption")
                return original_append(journal, **kwargs)

            with patch.object(live_module, "NativeBuildExecutor") as build_cls, patch.object(
                receipt_module.WriteAheadJournal, "append", new=fail_receipt_completed
            ):
                build_cls.return_value.run.return_value = build
                with self.assertRaises(OSError):
                    LiveInstallExecutor(
                        plan=plan, registry=registry, context=context, paths=paths,
                        source_root=_bootstrap.REPO_ROOT, runner=runner, snapshot=snapshot,
                        decision_selector=lambda decision: FinalAction.KEEP,
                        package_actions_applied=True, allow_unprivileged_system_commit=True,
                        privileged_uid=os.getuid(), privileged_gid=os.getgid(),
                        persist_diagnostic_on_failure=False,
                    ).run()
            self.assertFalse(receipt_path.exists())
            self.assertEqual(context.transaction.state, TransactionState.ROLLED_BACK)
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["trigger"], "live_install_exception")

    def test_rollback_failure_is_manual_attention_and_never_reported_clean(self) -> None:
        from tests.installer.test_live_install import Phase16LiveInstallTests
        from realmheart_installer.live import orchestrator as live_module
        from realmheart_installer.live.backend import LiveMutationBackend
        from realmheart_installer.live.orchestrator import LiveInstallExecutor

        with self._tempdir() as temp:
            root = Path(temp)
            fixture = Phase16LiveInstallTests(methodName="test_full_fake_root_failed_essential_probe_rolls_back_exactly_and_keeps_no_receipt")
            paths, snapshot, registry, plan, build, runner, context = fixture._fixture(root, fail_event_ping=True)
            with patch.object(live_module, "NativeBuildExecutor") as build_cls, patch.object(
                LiveMutationBackend, "rollback_all", return_value=("synthetic rollback failure",)
            ):
                build_cls.return_value.run.return_value = build
                result = LiveInstallExecutor(
                    plan=plan, registry=registry, context=context, paths=paths,
                    source_root=_bootstrap.REPO_ROOT, runner=runner, snapshot=snapshot,
                    decision_selector=lambda decision: FinalAction.RESTORE_PREVIOUS,
                    package_actions_applied=True, allow_unprivileged_system_commit=True,
                    privileged_uid=os.getuid(), privileged_gid=os.getgid(),
                    persist_diagnostic_on_failure=False,
                ).run()
            self.assertEqual(result.finalization.exit_code, 22)
            self.assertEqual(context.transaction.state, TransactionState.ROLLBACK_FAILED)
            payload = json.loads((context.transaction_dir / "recovery.json").read_text())
            self.assertEqual(payload["status"], "manual_attention")
            self.assertIn("synthetic rollback failure", payload["rollback_errors"])


if __name__ == "__main__":
    unittest.main()
