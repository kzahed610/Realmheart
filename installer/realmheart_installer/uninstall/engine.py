"""Transactional Phase-17 uninstall executor."""
from __future__ import annotations

import os
import shutil
import stat
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from ..configuration.terminal import KITTY_BEGIN, KITTY_END
from ..context import InstallContext, XdgPaths
from ..environment.command import CommandRunner
from ..errors import InstallerError, interruption_reason, PreconditionFailedError
from ..filesystem.backup import _copy_path_symlink_safe, create_backup_snapshot
from ..filesystem.compare import fingerprint_path
from ..filesystem.managed_block import ManagedBlock, plan_remove_managed_block
from ..filesystem.staging import FullTreeSwap, prepare_full_tree_stage
from ..models import OperationSafety, OperationState, Reversibility, TransactionState
from ..package_manager.pacman import PacmanAdapter
from ..transaction.journal import WriteAheadJournal
from ..transaction.operations import (
    CreateDirectoryOperation,
    MovePathOperation,
    RemovePathOperation,
    TransactionExecutor,
    new_operation_id,
)
from ..transaction.preconditions import capture_path_precondition
from ..transaction.recovery import persist_recovery_report
from .models import (
    BaselineEntry,
    FootprintAction,
    FootprintEntry,
    PackageCleanupResult,
    ServiceState,
    UninstallConfigAction,
    UninstallPlan,
    UninstallResult,
)


@dataclass
class _Mutation:
    rollback: Callable[[], None]
    finalize: Callable[[], None] = lambda: None


@dataclass(frozen=True)
class _RuntimeUnitState:
    enabled: bool
    active: bool


class UninstallExecutor:
    def __init__(
        self,
        *,
        plan: UninstallPlan,
        context: InstallContext,
        paths: XdgPaths,
        runner: CommandRunner,
        config_action: UninstallConfigAction,
        cleanup_dependencies: bool = False,
        purge_event_history: bool = False,
        prefix: Path = Path("/usr/local"),
        sysconf: Path = Path("/etc"),
        allow_unprivileged_system_mutation: bool = False,
    ) -> None:
        self.plan = plan
        self.context = context
        self.paths = paths
        self.runner = runner
        self.config_action = config_action
        self.cleanup_dependencies = cleanup_dependencies
        self.purge_event_history = purge_event_history
        self.prefix = Path(prefix)
        self.sysconf = Path(sysconf)
        self.allow_unprivileged_system_mutation = allow_unprivileged_system_mutation
        self.journal = WriteAheadJournal(context.journal_path)
        self.executor = TransactionExecutor(self.journal)
        self._mutations: list[_Mutation] = []
        self._removed: list[str] = []
        self._restored: list[str] = []
        self._preserved: list[str] = list(plan.preserved_paths)
        self._warnings: list[str] = list(plan.warnings)
        self._errors: list[str] = []
        self._start_service_state: dict[str, _RuntimeUnitState] = {}

    def run(self) -> UninstallResult:
        self.context.ensure_recovery_reserve()
        if not self.plan.ready:
            raise InstallerError(
                "uninstall plan is blocked: " + "; ".join(self.plan.blockers),
                code="RH_UNINSTALL_PLAN_BLOCKED",
                stage="uninstall",
            )
        if self.config_action is UninstallConfigAction.RESTORE_BASELINE:
            if not self.plan.baseline_available or not self.plan.baseline_valid:
                raise InstallerError(
                    "pre-Realmheart baseline restoration was requested but no valid baseline is available",
                    code="RH_UNINSTALL_BASELINE_UNAVAILABLE",
                    stage="uninstall",
                )

        self.context.transaction.metadata["scope"] = "phase17_uninstall"
        self.context.transaction.metadata["uninstall_config_action"] = self.config_action.value
        self.context.transaction.metadata["install_transaction_id"] = self.plan.install_transaction_id
        self.context.transaction.current_version = self.plan.installed_version
        self.context.transaction.installation_origin = "managed_installer"
        self.context.transaction.transition(TransactionState.PLANNED)
        self.context.persist_json("uninstall-plan.json", self.plan)
        self.context.persist_summary()

        safety_snapshot: str | None = None
        package_result: PackageCleanupResult | None = None
        receipt_retired_to: str | None = None
        rolled_back = False

        try:
            if self.config_action is UninstallConfigAction.RESTORE_BASELINE and self.plan.has_config_divergence:
                self.context.transaction.transition(TransactionState.BACKUP)
                self.context.persist_summary()
                snapshot = self._create_pre_uninstall_snapshot()
                safety_snapshot = str(snapshot) if snapshot else None

            self.context.transaction.transition(TransactionState.APPLYING)
            self.context.persist_summary()

            self._quiesce_services()
            self._apply_configuration_choice()
            self._cleanup_managed_footprint()

            if self.purge_event_history:
                events = self.paths.realmheart_state / "events.db"
                if events.exists() or events.is_symlink():
                    self._remove_unprivileged(events, expected=fingerprint_path(events), label="event-history")
                    self._removed.append(str(events))
                try:
                    self._preserved.remove(str(events))
                except ValueError:
                    pass

            self._reload_and_restore_previous_services()

            # The receipt is the managed-install authority. Retire it only after
            # filesystem/service cleanup has completed, so a partial uninstall
            # cannot masquerade as a clean unmanaged state.
            unresolved = [item for item in self.plan.footprint if item.keep_current_action is FootprintAction.PRESERVE_CONFLICT and self.config_action is UninstallConfigAction.KEEP_CURRENT]
            if unresolved:
                names = ", ".join(item.target for item in unresolved)
                raise InstallerError(
                    "uninstall preserved installer-owned paths that changed after installation; authoritative receipt retained for safe retry: " + names,
                    code="RH_UNINSTALL_DIVERGED_FOOTPRINT",
                    stage="uninstall",
                )

            receipt_retired_to = self._retire_receipt()
            package_result = self._cleanup_packages_if_requested()

            self.context.transaction.metadata["uninstall_receipt_retired_to"] = receipt_retired_to
            if package_result is not None:
                self.context.transaction.metadata["package_cleanup"] = {
                    "requested": list(package_result.requested),
                    "removed": list(package_result.removed),
                    "error": package_result.error,
                }
            self.context.transaction.transition(TransactionState.COMMITTING)
            self.context.persist_summary()

            # The authoritative uninstall state becomes durable *before* deleting
            # transaction-old/preimage material.  A crash after COMMITTED may
            # leave cleanup debris, but it can never strand a nonterminal
            # transaction whose rollback material was already destroyed.
            self.context.transaction.transition(TransactionState.COMMITTED)
            self.context.persist_summary()

            for mutation in self._mutations:
                try:
                    mutation.finalize()
                except Exception as exc:
                    self._warnings.append(f"post-commit uninstall cleanup failed: {exc}")
            self._cleanup_empty_owned_parents()

            result = UninstallResult(
                transaction_id=self.plan.transaction_id,
                config_action=self.config_action,
                completed=True,
                rolled_back=False,
                safety_snapshot=safety_snapshot,
                receipt_retired_to=receipt_retired_to,
                removed_paths=tuple(dict.fromkeys(self._removed)),
                restored_paths=tuple(dict.fromkeys(self._restored)),
                preserved_paths=tuple(dict.fromkeys(self._preserved)),
                warnings=tuple(dict.fromkeys(self._warnings)),
                errors=(),
                package_cleanup=package_result,
                exit_code=0,
            )
            try:
                self.context.persist_json("uninstall-result.json", result)
            except Exception as exc:
                self._warnings.append(f"post-commit uninstall result persistence failed: {exc}")
                result = UninstallResult(
                    transaction_id=self.plan.transaction_id,
                    config_action=self.config_action,
                    completed=True,
                    rolled_back=False,
                    safety_snapshot=safety_snapshot,
                    receipt_retired_to=receipt_retired_to,
                    removed_paths=tuple(dict.fromkeys(self._removed)),
                    restored_paths=tuple(dict.fromkeys(self._restored)),
                    preserved_paths=tuple(dict.fromkeys(self._preserved)),
                    warnings=tuple(dict.fromkeys(self._warnings)),
                    errors=(),
                    package_cleanup=package_result,
                    exit_code=0,
                )
                persist_recovery_report(
                    self.context, trigger="uninstall_post_commit_reporting_failure", primary_error=exc,
                )
            else:
                self.context.release_recovery_reserve()
            return result
        except KeyboardInterrupt as exc:
            reason = interruption_reason(exc)
            if self.context.transaction.state not in {
                TransactionState.COMMITTED, TransactionState.ROLLED_BACK, TransactionState.ROLLBACK_FAILED
            }:
                self.context.transaction.transition(TransactionState.INTERRUPTED)
                self.context.transaction.metadata["interruption"] = reason
            else:
                self.context.transaction.metadata["interruption_after_terminal"] = reason
            try:
                self.context.persist_summary()
            except Exception:
                pass
            persist_recovery_report(self.context, trigger="uninstall_interruption", primary_error=exc)
            raise
        except Exception as exc:
            self._errors.append(str(exc))
            self.context.transaction.transition(TransactionState.ROLLING_BACK)
            self.context.persist_summary()
            rollback_errors = self._rollback_all()
            rolled_back = not rollback_errors
            self._errors.extend(rollback_errors)
            self._restore_uninstall_start_services()
            self.context.transaction.transition(TransactionState.ROLLED_BACK if rolled_back else TransactionState.ROLLBACK_FAILED)
            self.context.transaction.metadata["uninstall_error"] = str(exc)
            try:
                self.context.persist_summary()
            except Exception:
                pass
            persist_recovery_report(
                self.context, trigger="uninstall_exception", primary_error=exc, rollback_errors=rollback_errors,
            )
            result = UninstallResult(
                transaction_id=self.plan.transaction_id,
                config_action=self.config_action,
                completed=False,
                rolled_back=rolled_back,
                safety_snapshot=safety_snapshot,
                receipt_retired_to=None,
                removed_paths=tuple(dict.fromkeys(self._removed)),
                restored_paths=tuple(dict.fromkeys(self._restored)),
                preserved_paths=tuple(dict.fromkeys(self._preserved)),
                warnings=tuple(dict.fromkeys(self._warnings)),
                errors=tuple(dict.fromkeys(self._errors)),
                package_cleanup=package_result,
                exit_code=31 if rolled_back else 32,
            )
            self.context.persist_json("uninstall-result.json", result)
            return result

    # ---- high-level policy -------------------------------------------------

    def _apply_configuration_choice(self) -> None:
        baseline = {entry.target: entry for entry in self.plan.baseline_entries}
        hypr_target = str(self.paths.config_home / "hypr")
        kitty_target = str(self.paths.config_home / "kitty/kitty.conf")

        if self.config_action is UninstallConfigAction.RESTORE_BASELINE:
            hypr = baseline.get(hypr_target)
            if hypr is not None:
                self._restore_hypr_baseline(hypr)
            kitty = baseline.get(kitty_target)
            if kitty is not None:
                self._restore_baseline_entry(kitty, expected=self._planned_current_fingerprint(kitty_target))
            return

        # KEEP_CURRENT intentionally leaves the current Hyprland tree untouched
        # but removes Realmheart's one managed Kitty include surgically.
        kitty = Path(kitty_target)
        operation = plan_remove_managed_block(
            target=kitty,
            allowed_root=self.paths.config_home,
            preimage_dir=self.context.preimage_dir / "uninstall-kitty",
            block=ManagedBlock(KITTY_BEGIN, KITTY_END, ""),
        )
        if operation is not None:
            self.executor.execute(operation)
            self._mutations.append(_Mutation(lambda op=operation: self.executor.rollback(op)))
            self._restored.append(kitty_target + " (Realmheart managed block removed)")

    def _cleanup_managed_footprint(self) -> None:
        special = {
            str(self.paths.config_home / "hypr"),
            str(self.paths.config_home / "kitty/kitty.conf"),
        }
        for entry in self.plan.footprint:
            if entry.target in special or entry.artifact_id == "realmheart.config":
                continue
            if not entry.current_exists and not (entry.baseline_existed and entry.baseline_backup_path):
                continue

            action = entry.keep_current_action
            if self.config_action is UninstallConfigAction.RESTORE_BASELINE:
                action = FootprintAction.RESTORE_PREIMAGE if entry.baseline_existed else FootprintAction.REMOVE

            if action is FootprintAction.PRESERVE:
                self._preserved.append(entry.target)
                continue
            if action is FootprintAction.PRESERVE_CONFLICT:
                self._preserved.append(entry.target)
                self._warnings.append(f"preserved diverged managed path: {entry.target}")
                continue

            current = Path(entry.target)
            if current.exists() or current.is_symlink():
                current_fp = fingerprint_path(current)
                # In restore-baseline mode a safety snapshot protects deliberate
                # replacement, but still refuse TOCTOU changes after the plan.
                if current_fp != entry.current_fingerprint:
                    raise PreconditionFailedError(entry.target, "path changed after uninstall planning")

            if action is FootprintAction.RESTORE_PREIMAGE:
                baseline = self._baseline_for_target(entry.target)
                if baseline is None or not baseline.existed or not baseline.backup_path:
                    raise InstallerError(
                        f"baseline preimage required but unavailable for {entry.target}",
                        code="RH_UNINSTALL_PREIMAGE_MISSING",
                        stage="uninstall",
                    )
                if entry.privileged and not self.allow_unprivileged_system_mutation:
                    self._privileged_restore(entry, baseline)
                else:
                    self._restore_baseline_entry(baseline, expected=entry.current_fingerprint)
                self._restored.append(entry.target)
                continue

            if action is FootprintAction.REMOVE:
                if not (current.exists() or current.is_symlink()):
                    continue
                if entry.privileged and not self.allow_unprivileged_system_mutation:
                    self._privileged_remove(entry)
                else:
                    self._remove_unprivileged(current, expected=entry.current_fingerprint, label=entry.artifact_id)
                self._removed.append(entry.target)

    # ---- baseline / exact filesystem mutations ----------------------------

    def _restore_hypr_baseline(self, entry: BaselineEntry) -> None:
        target = Path(entry.target)
        expected = self._planned_current_fingerprint(entry.target)
        if not entry.existed:
            if target.exists() or target.is_symlink():
                self._remove_unprivileged(target, expected=expected, label="hypr-baseline-absent")
                self._removed.append(str(target))
            return
        if not entry.backup_path:
            raise InstallerError("Hyprland baseline record has no backup path", code="RH_UNINSTALL_PREIMAGE_MISSING", stage="uninstall")
        backup = Path(entry.backup_path)
        if not backup.is_dir() or backup.is_symlink():
            raise InstallerError("Hyprland baseline preimage is not a normal directory", code="RH_UNINSTALL_PREIMAGE_INVALID", stage="uninstall")
        self._ensure_user_parents(target.parent)
        stage = prepare_full_tree_stage(
            release_tree=backup,
            target=target,
            transaction_id=self.plan.transaction_id + "-uninstall",
            preserve_relative_paths=(),
        )
        if expected is not None and stage.active_precondition_fingerprint != expected:
            _safe_remove(stage.staging)
            raise PreconditionFailedError(str(target), "Hyprland tree changed after uninstall planning")
        if entry.baseline_fingerprint and stage.staging_fingerprint != entry.baseline_fingerprint:
            _safe_remove(stage.staging)
            raise InstallerError("staged Hyprland baseline identity mismatch", code="RH_UNINSTALL_BASELINE_DRIFT", stage="uninstall")
        swap = FullTreeSwap(stage, self.journal)
        try:
            swap.execute()
        except Exception:
            if swap.operations:
                swap.rollback()
            _safe_remove(stage.staging)
            raise
        self._mutations.append(_Mutation(
            rollback=lambda swap=swap, stage=stage: (swap.rollback(), _safe_remove(stage.staging)),
            finalize=lambda stage=stage: _safe_remove(stage.old),
        ))
        self._restored.append(str(target))

    def _restore_baseline_entry(self, entry: BaselineEntry, *, expected: str | None) -> None:
        target = Path(entry.target)
        if not entry.existed:
            if target.exists() or target.is_symlink():
                self._remove_unprivileged(target, expected=expected or fingerprint_path(target), label=entry.label)
                self._removed.append(str(target))
            return
        if not entry.backup_path:
            raise InstallerError(f"baseline preimage missing for {entry.target}", code="RH_UNINSTALL_PREIMAGE_MISSING", stage="uninstall")
        self._restore_unprivileged_exact(target, Path(entry.backup_path), expected=expected, expected_baseline=entry.baseline_fingerprint, label=entry.label)
        self._restored.append(str(target))

    def _restore_unprivileged_exact(self, target: Path, backup: Path, *, expected: str | None, expected_baseline: str | None, label: str) -> None:
        if target.exists() or target.is_symlink():
            if expected is not None and fingerprint_path(target) != expected:
                raise PreconditionFailedError(str(target), "path changed after uninstall planning")
        self._ensure_user_parents(target.parent)
        staging = target.parent / f".{target.name}.rh-uninstall-restore-{self.plan.transaction_id}-{_token(label)}"
        old = target.parent / f".{target.name}.rh-uninstall-old-{self.plan.transaction_id}-{_token(label)}"
        if staging.exists() or staging.is_symlink() or old.exists() or old.is_symlink():
            raise InstallerError(f"uninstall staging collision for {target}", code="RH_UNINSTALL_STAGING_COLLISION", stage="uninstall")
        _copy_path_symlink_safe(backup, staging)
        if expected_baseline and fingerprint_path(staging) != expected_baseline:
            _safe_remove(staging)
            raise InstallerError(f"baseline preimage identity mismatch for {target}", code="RH_UNINSTALL_BASELINE_DRIFT", stage="uninstall")

        operations: list[MovePathOperation] = []
        allowed = self._allowed_user_root(target)
        try:
            if target.exists() or target.is_symlink():
                move_old = MovePathOperation(
                    target=target,
                    destination=old,
                    allowed_root=allowed,
                    safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(target), str(old)),
                )
                self.executor.execute(move_old)
                operations.append(move_old)
            activate = MovePathOperation(
                target=staging,
                destination=target,
                allowed_root=allowed,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(staging), str(staging)),
            )
            self.executor.execute(activate)
            operations.append(activate)
        except Exception:
            for operation in reversed(operations):
                self.executor.rollback(operation)
            _safe_remove(staging)
            raise

        def rollback() -> None:
            for operation in reversed(operations):
                self.executor.rollback(operation)
            _safe_remove(staging)

        self._mutations.append(_Mutation(rollback=rollback, finalize=lambda old=old: _safe_remove(old)))

    def _remove_unprivileged(self, target: Path, *, expected: str, label: str) -> None:
        if fingerprint_path(target) != expected:
            raise PreconditionFailedError(str(target), "path changed after uninstall planning")
        allowed = self._allowed_user_root(target)
        backup = target.parent / f".{target.name}.rh-uninstall-old-{self.plan.transaction_id}-{_token(label)}"
        if backup.exists() or backup.is_symlink():
            raise InstallerError(f"uninstall removal collision for {target}", code="RH_UNINSTALL_STAGING_COLLISION", stage="uninstall")
        op = MovePathOperation(
            target=target,
            destination=backup,
            allowed_root=allowed,
            safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(target), str(backup)),
        )
        self.executor.execute(op)
        self._mutations.append(_Mutation(lambda op=op: self.executor.rollback(op), lambda backup=backup: _safe_remove(backup)))

    def _ensure_user_parents(self, directory: Path) -> None:
        allowed = self._allowed_user_root(directory)
        missing: list[Path] = []
        cursor = directory
        while not (cursor.exists() or cursor.is_symlink()):
            if cursor == allowed:
                break
            missing.append(cursor)
            cursor = cursor.parent
        if cursor.is_symlink():
            raise InstallerError(f"refusing uninstall path beneath symlinked parent {cursor}", code="RH_UNINSTALL_SYMLINK_PARENT", stage="uninstall")
        for path in reversed(missing):
            op = CreateDirectoryOperation(
                target=path,
                allowed_root=allowed,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(path), None),
                mode=0o700,
            )
            self.executor.execute(op)
            self._mutations.append(_Mutation(lambda op=op: self.executor.rollback(op)))

    # ---- privileged exact mutations ---------------------------------------

    def _privileged_remove(self, entry: FootprintEntry) -> None:
        target = Path(entry.target)
        self._assert_secure_privileged_parent(target)
        if fingerprint_path(target) != entry.current_fingerprint:
            raise PreconditionFailedError(entry.target, "privileged path changed after uninstall planning")
        sudo, tools = self._privileged_tools()
        old = target.parent / f".{target.name}.rh-uninstall-old-{self.plan.transaction_id}-{_token(entry.artifact_id)}"
        if old.exists() or old.is_symlink():
            raise InstallerError(f"privileged uninstall sibling collision: {old}", code="RH_UNINSTALL_STAGING_COLLISION", stage="uninstall")
        op_id = new_operation_id()
        self.journal.append(operation_id=op_id, state=OperationState.INTENT, kind="uninstall_privileged_remove", target=str(target), data={"before_fingerprint": entry.current_fingerprint, "old": str(old), "artifact_id": entry.artifact_id})
        self.journal.append(operation_id=op_id, state=OperationState.STARTED, kind="uninstall_privileged_remove", target=str(target))
        try:
            self._sudo(sudo, tools["mv"], "--", str(target), str(old))
        except Exception as exc:
            self.journal.append(operation_id=op_id, state=OperationState.FAILED, kind="uninstall_privileged_remove", target=str(target), data={"error": str(exc)})
            raise
        self.journal.append(operation_id=op_id, state=OperationState.COMPLETED, kind="uninstall_privileged_remove", target=str(target))

        def rollback() -> None:
            self.journal.append(operation_id=op_id, state=OperationState.ROLLBACK_STARTED, kind="uninstall_privileged_remove", target=str(target))
            if target.exists() or target.is_symlink():
                raise PreconditionFailedError(str(target), "privileged rollback target became occupied")
            self._sudo(sudo, tools["mv"], "--", str(old), str(target))
            self.journal.append(operation_id=op_id, state=OperationState.ROLLED_BACK, kind="uninstall_privileged_remove", target=str(target))

        self._mutations.append(_Mutation(rollback, lambda: self._sudo_if_exists(sudo, tools["rm"], old)))

    def _privileged_restore(self, entry: FootprintEntry, baseline: BaselineEntry) -> None:
        target = Path(entry.target)
        self._assert_secure_privileged_parent(target)
        if target.exists() or target.is_symlink():
            if fingerprint_path(target) != entry.current_fingerprint:
                raise PreconditionFailedError(entry.target, "privileged path changed after uninstall planning")
        if not baseline.backup_path or not baseline.baseline_fingerprint:
            raise InstallerError(f"privileged baseline preimage is incomplete for {entry.target}", code="RH_UNINSTALL_PREIMAGE_MISSING", stage="uninstall")
        if baseline.source_type != "file" or baseline.source_mode is None or baseline.source_uid is None or baseline.source_gid is None:
            raise InstallerError(f"privileged baseline metadata is incomplete for {entry.target}", code="RH_UNINSTALL_PRIVILEGED_METADATA_MISSING", stage="uninstall")
        backup = Path(baseline.backup_path)
        sudo, tools = self._privileged_tools()
        token = _token(entry.artifact_id)
        candidate = target.parent / f".{target.name}.rh-uninstall-restore-{self.plan.transaction_id}-{token}"
        old = target.parent / f".{target.name}.rh-uninstall-old-{self.plan.transaction_id}-{token}"
        for path in (candidate, old):
            if path.exists() or path.is_symlink():
                raise InstallerError(f"privileged uninstall sibling collision: {path}", code="RH_UNINSTALL_STAGING_COLLISION", stage="uninstall")
        existed = target.exists() or target.is_symlink()
        op_id = new_operation_id()
        self.journal.append(operation_id=op_id, state=OperationState.INTENT, kind="uninstall_privileged_restore", target=str(target), data={"before_fingerprint": entry.current_fingerprint, "baseline_fingerprint": baseline.baseline_fingerprint, "candidate": str(candidate), "old": str(old), "artifact_id": entry.artifact_id})
        self.journal.append(operation_id=op_id, state=OperationState.STARTED, kind="uninstall_privileged_restore", target=str(target))
        try:
            self._sudo(sudo, tools["cp"], "-a", "--", str(backup), str(candidate))
            self._sudo(sudo, tools["chown"], f"{baseline.source_uid}:{baseline.source_gid}", "--", str(candidate))
            self._sudo(sudo, tools["chmod"], baseline.source_mode, "--", str(candidate))
            if existed:
                self._sudo(sudo, tools["mv"], "--", str(target), str(old))
            self._sudo(sudo, tools["mv"], "--", str(candidate), str(target))
            if fingerprint_path(target) != baseline.baseline_fingerprint:
                raise InstallerError(f"restored privileged baseline identity mismatch for {target}", code="RH_UNINSTALL_BASELINE_DRIFT", stage="uninstall")
        except Exception as exc:
            self.journal.append(operation_id=op_id, state=OperationState.FAILED, kind="uninstall_privileged_restore", target=str(target), data={"error": str(exc)})
            self._rollback_privileged_restore_partial(sudo, tools, target, candidate, old, existed, baseline.baseline_fingerprint)
            raise
        self.journal.append(operation_id=op_id, state=OperationState.COMPLETED, kind="uninstall_privileged_restore", target=str(target), data={"after_fingerprint": baseline.baseline_fingerprint})

        def rollback() -> None:
            self.journal.append(operation_id=op_id, state=OperationState.ROLLBACK_STARTED, kind="uninstall_privileged_restore", target=str(target))
            self._rollback_privileged_restore_partial(sudo, tools, target, candidate, old, existed, baseline.baseline_fingerprint)
            self.journal.append(operation_id=op_id, state=OperationState.ROLLED_BACK, kind="uninstall_privileged_restore", target=str(target))

        self._mutations.append(_Mutation(rollback, lambda: self._sudo_if_exists(sudo, tools["rm"], old)))

    def _rollback_privileged_restore_partial(self, sudo: str, tools: dict[str, str], target: Path, candidate: Path, old: Path, existed: bool, expected_after: str) -> None:
        if target.exists() or target.is_symlink():
            if old.exists() or old.is_symlink():
                if fingerprint_path(target) != expected_after:
                    raise PreconditionFailedError(str(target), "restored privileged path changed before rollback")
                self._sudo(sudo, tools["rm"], "-rf", "--", str(target))
                self._sudo(sudo, tools["mv"], "--", str(old), str(target))
            elif not existed:
                self._sudo(sudo, tools["rm"], "-rf", "--", str(target))
        elif old.exists() or old.is_symlink():
            self._sudo(sudo, tools["mv"], "--", str(old), str(target))
        if candidate.exists() or candidate.is_symlink():
            self._sudo(sudo, tools["rm"], "-rf", "--", str(candidate))

    # ---- service state -----------------------------------------------------

    def _service_names(self) -> tuple[str, ...]:
        names = {
            Path(item.target).name
            for item in self.plan.footprint
            if item.artifact_type == "service" and Path(item.target).name.endswith((".service", ".path"))
        }
        return tuple(sorted(names))

    def _quiesce_services(self) -> None:
        names = self._service_names()
        if not names:
            return
        systemctl = self.runner.which("systemctl")
        if not systemctl:
            self._warnings.append("systemctl is unavailable; Realmheart user units could not be quiesced before file cleanup")
            return
        for name in names:
            state = self._unit_state(systemctl, name)
            self._start_service_state[name] = state
            if not (state.enabled or state.active):
                continue
            # Disable first so a stopped unit is not reactivated by normal target
            # dependencies while its files are being removed/restored.  A known
            # running/enabled Realmheart unit that cannot be quiesced is a hard
            # uninstall safety failure: do not remove its executable/unit files
            # from underneath it and pretend cleanup succeeded.
            result = self.runner.run((systemctl, "--user", "disable", "--now", name), timeout=12.0, interactive=True)
            if not result.ok:
                detail = (result.stderr or result.stdout).strip() or f"exit {result.returncode}"
                raise InstallerError(
                    f"could not quiesce Realmheart user unit {name}: {detail}",
                    code="RH_UNINSTALL_SERVICE_QUIESCE_FAILED",
                    stage="uninstall",
                )

    def _reload_and_restore_previous_services(self) -> None:
        names = self._service_names()
        if not names:
            return
        systemctl = self.runner.which("systemctl")
        if not systemctl:
            return
        reload_result = self.runner.run((systemctl, "--user", "daemon-reload"), timeout=10.0)
        if not reload_result.ok:
            self._warnings.append("systemd user daemon-reload failed during uninstall")
            return
        prior = {item.service: item for item in self.plan.service_states}
        baseline_targets = {entry.target: entry for entry in self.plan.baseline_entries}
        for name in names:
            unit_path = str(self.paths.config_home / "systemd/user" / name)
            baseline = baseline_targets.get(unit_path)
            old_state = prior.get(name)
            if not baseline or not baseline.existed or old_state is None:
                continue
            if old_state.enabled_before_install:
                self.runner.run((systemctl, "--user", "enable", name), timeout=10.0, interactive=True)
            if old_state.active_before_install:
                self.runner.run((systemctl, "--user", "start", name), timeout=10.0, interactive=True)

    def _restore_uninstall_start_services(self) -> None:
        if not self._start_service_state:
            return
        systemctl = self.runner.which("systemctl")
        if not systemctl:
            return
        self.runner.run((systemctl, "--user", "daemon-reload"), timeout=10.0)
        for name, state in self._start_service_state.items():
            if state.enabled:
                self.runner.run((systemctl, "--user", "enable", name), timeout=10.0, interactive=True)
            else:
                self.runner.run((systemctl, "--user", "disable", name), timeout=10.0, interactive=True)
            if state.active:
                self.runner.run((systemctl, "--user", "start", name), timeout=10.0, interactive=True)
            else:
                self.runner.run((systemctl, "--user", "stop", name), timeout=10.0, interactive=True)

    def _unit_state(self, systemctl: str, service: str) -> _RuntimeUnitState:
        enabled = self.runner.run((systemctl, "--user", "is-enabled", service), timeout=4.0)
        active = self.runner.run((systemctl, "--user", "is-active", service), timeout=4.0)
        return _RuntimeUnitState(enabled.ok and enabled.stdout.strip() not in {"disabled", "masked", "not-found"}, active.ok and active.stdout.strip() == "active")

    # ---- receipt/packages/final cleanup -----------------------------------

    def _retire_receipt(self) -> str | None:
        target = Path(self.plan.receipt_path)
        if not (target.exists() or target.is_symlink()):
            return None
        backup_dir = self.context.preimage_dir / "uninstall-receipt"
        backup_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        backup = backup_dir / "previous-installed-state.json"
        if backup.exists() or backup.is_symlink():
            raise InstallerError("uninstall receipt archive path already exists", code="RH_UNINSTALL_RECEIPT_COLLISION", stage="uninstall")
        op = RemovePathOperation(
            target=target,
            allowed_root=self.paths.state_home,
            safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(target), str(backup)),
            backup_path=backup,
        )
        self.executor.execute(op)
        self._mutations.append(_Mutation(lambda op=op: self.executor.rollback(op)))
        return str(backup)

    def _cleanup_packages_if_requested(self) -> PackageCleanupResult | None:
        candidates = self.plan.package_cleanup_candidates
        if not candidates:
            return None
        requested = tuple(item.package for item in candidates)
        if not self.cleanup_dependencies:
            self._warnings.append("packages installed by the historical Realmheart transaction were retained; dependency cleanup is opt-in")
            return PackageCleanupResult(requested, (), None)
        adapter = PacmanAdapter(self.runner)
        self.context.transaction.metadata["package_cleanup_started"] = True
        self.context.persist_summary()
        result = adapter.remove_exact(requested)
        if result.error:
            self._warnings.append("optional package cleanup was incomplete: " + result.error)
        return PackageCleanupResult(result.requested, result.removed, result.error)

    def _create_pre_uninstall_snapshot(self) -> Path | None:
        changed = [item for item in self.plan.comparisons if item.changed]
        if not changed:
            return None
        sources: dict[str, Path] = {}
        for index, comparison in enumerate(changed, start=1):
            sources[f"current-{index:03d}"] = Path(comparison.target)
        destination = self.paths.version_backups / f"pre-uninstall-{self.plan.transaction_id}"
        return create_backup_snapshot(
            destination,
            sources,
            snapshot_kind="pre_uninstall_safety",
            installer_version=self.context.transaction.installer_version,
            target_realmheart_version=self.plan.installed_version or "unknown",
            transaction_id=self.plan.transaction_id,
        )

    def _rollback_all(self) -> tuple[str, ...]:
        errors: list[str] = []
        for mutation in reversed(self._mutations):
            try:
                mutation.rollback()
            except Exception as exc:
                errors.append(str(exc))
        return tuple(errors)

    def _cleanup_empty_owned_parents(self) -> None:
        # Conservative rmdir only. Never recursively remove shared config/state
        # roots, and never treat failure/non-empty as an uninstall error.
        candidates = [
            self.paths.state_home / "realmheart/theme",
            self.paths.config_home / "realmheart/scripts/terminal",
            self.prefix / "lib/realmheart",
            self.prefix / "libexec/realmheart",
            self.prefix / "share/realmheart",
        ]
        for path in candidates:
            try:
                path.rmdir()
            except OSError:
                pass

    # ---- helpers -----------------------------------------------------------

    def _baseline_for_target(self, target: str) -> BaselineEntry | None:
        return next((item for item in self.plan.baseline_entries if item.target == target), None)

    def _planned_current_fingerprint(self, target: str) -> str | None:
        entry = next((item for item in self.plan.footprint if item.target == target), None)
        if entry is not None:
            return entry.current_fingerprint
        comparison = next((item for item in self.plan.comparisons if item.target == target), None)
        return comparison.current_fingerprint if comparison is not None else None

    def _allowed_user_root(self, target: Path) -> Path:
        absolute = Path(os.path.abspath(target))
        for root in (self.paths.config_home, self.paths.state_home, self.paths.data_home, self.paths.home):
            root_abs = Path(os.path.abspath(root))
            if _within(absolute, root_abs):
                return root_abs
        if self.allow_unprivileged_system_mutation:
            for root in (self.prefix, self.sysconf):
                root_abs = Path(os.path.abspath(root))
                if _within(absolute, root_abs):
                    return root_abs
        raise InstallerError(f"uninstall target is outside allowed unprivileged roots: {target}", code="RH_UNINSTALL_UNSAFE_PATH", stage="uninstall")

    def _assert_secure_privileged_parent(self, target: Path) -> None:
        if not target.is_absolute():
            raise InstallerError(f"privileged uninstall target is not absolute: {target}", code="RH_UNINSTALL_UNSAFE_PATH", stage="uninstall")
        if not (_within(target, self.prefix) or _within(target, self.sysconf)):
            raise InstallerError(f"privileged uninstall target is outside approved roots: {target}", code="RH_UNINSTALL_UNSAFE_PATH", stage="uninstall")
        current = target.parent
        while True:
            if current.exists() or current.is_symlink():
                st = current.lstat()
                if stat.S_ISLNK(st.st_mode):
                    raise InstallerError(f"privileged uninstall parent is a symlink: {current}", code="RH_UNINSTALL_INSECURE_PARENT", stage="uninstall")
                if not stat.S_ISDIR(st.st_mode):
                    raise InstallerError(f"privileged uninstall parent is not a directory: {current}", code="RH_UNINSTALL_INSECURE_PARENT", stage="uninstall")
                if stat.S_IMODE(st.st_mode) & 0o022:
                    raise InstallerError(f"privileged uninstall parent is group/world-writable: {current}", code="RH_UNINSTALL_INSECURE_PARENT", stage="uninstall")
            if current == current.parent:
                break
            current = current.parent

    def _privileged_tools(self) -> tuple[str, dict[str, str]]:
        sudo = self.runner.which("sudo")
        if not sudo:
            raise InstallerError("sudo is required for privileged Realmheart artifact uninstall", code="RH_UNINSTALL_SUDO_MISSING", stage="uninstall")
        tools: dict[str, str] = {}
        for name in ("mv", "cp", "rm", "chown", "chmod"):
            value = self.runner.which(name)
            if not value:
                raise InstallerError(f"required privileged file tool is missing: {name}", code="RH_UNINSTALL_TOOL_MISSING", stage="uninstall")
            tools[name] = value
        return sudo, tools

    def _sudo(self, sudo: str, tool: str, *args: str) -> None:
        result = self.runner.run((sudo, tool, *args), timeout=None, interactive=True)
        if not result.ok:
            raise InstallerError(f"privileged uninstall command failed: {tool} {' '.join(args[:2])}", code="RH_UNINSTALL_PRIVILEGED_COMMAND_FAILED", stage="uninstall")

    def _sudo_if_exists(self, sudo: str, rm_tool: str, path: Path) -> None:
        if path.exists() or path.is_symlink():
            self._sudo(sudo, rm_tool, "-rf", "--", str(path))


def _within(path: Path, root: Path) -> bool:
    try:
        Path(os.path.abspath(path)).relative_to(Path(os.path.abspath(root)))
        return True
    except ValueError:
        return False


def _token(value: str) -> str:
    safe = "".join(ch if ch.isalnum() else "-" for ch in value).strip("-")
    return safe[:48] or "path"


def _safe_remove(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink(missing_ok=True)
    elif path.is_dir():
        shutil.rmtree(path)
