"""Concrete Phase-16 live mutation backend.

The component layer decides *what* belongs to each logical subsystem.  This
backend performs the approved artifact/config/service actions through the
transaction kernel.  System paths use narrowly-scoped sudo commands rather than
running the installer or CMake as root.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import stat
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from ..components.models import HandlerStepResult, RollbackRequirement
from ..configuration import ConfigurationIntegrator
from ..configuration.terminal import render_action_content
from ..context import InstallContext, XdgPaths
from ..environment.command import CommandRunner
from ..errors import InstallerError, OperationExecutionError, PreconditionFailedError
from ..filesystem.compare import fingerprint_path, fingerprint_regular_bytes
from ..filesystem.backup import validate_backup_snapshot
from ..filesystem.staging import FullTreeSwap, prepare_full_tree_stage
from ..models import MutationPrecondition, OperationSafety, OperationState, Reversibility
from ..native_build.models import BuildStageReport, StagedArtifactResult
from ..planning.models import (
    ArtifactAction,
    ConfigAction,
    ConfigActionKind,
    InstallationPlan,
    PlannedComponent,
    PlannedHealthCheck,
    ServiceAction,
    ServiceActionKind,
)
from ..transaction.journal import WriteAheadJournal
from ..transaction.operations import (
    CreateDirectoryOperation,
    RemovePathOperation,
    TransactionExecutor,
    WriteFileOperation,
    new_operation_id,
)
from ..transaction.preconditions import FINGERPRINT_PRECONDITION, capture_path_precondition


@dataclass
class _RollbackGroup:
    group_id: str
    kind: str
    rollback_fn: Callable[[], None]
    finalize_fn: Callable[[], None] = lambda: None
    quiesce_fn: Callable[[], None] = lambda: None
    restore_fn: Callable[[], None] = lambda: None
    rolled_back: bool = False
    finalized: bool = False

    def quiesce(self) -> None:
        if not self.rolled_back:
            self.quiesce_fn()

    def rollback(self) -> None:
        if not self.rolled_back:
            self.rollback_fn()
            self.rolled_back = True

    def restore(self) -> None:
        if self.rolled_back:
            self.restore_fn()

    def finalize(self) -> None:
        if not self.finalized and not self.rolled_back:
            self.finalize_fn()
            self.finalized = True


@dataclass(frozen=True)
class _UnitState:
    enabled: bool
    active: bool


class LiveMutationBackend:
    """Actual ComponentMutationBackend used by the final live transaction.

    ``allow_unprivileged_system_commit`` exists only for fake-root tests.  The
    production CLI never enables it for /usr/local or /etc.
    """

    def __init__(
        self,
        *,
        plan: InstallationPlan,
        build_report: BuildStageReport,
        context: InstallContext,
        paths: XdgPaths,
        source_root: Path,
        runner: CommandRunner,
        allow_unprivileged_system_commit: bool = False,
        activate_user_services: bool = True,
    ) -> None:
        if not build_report.ok:
            raise ValueError("live backend requires a validated build-stage report")
        if build_report.transaction_id != plan.transaction_id:
            raise ValueError("build report/plan transaction identity mismatch")
        if build_report.provenance is None or build_report.provenance.plan_digest != plan.plan_digest:
            raise ValueError("build report provenance does not match approved InstallationPlan")
        self.plan = plan
        self.build_report = build_report
        self.context = context
        self.paths = paths
        self.source_root = Path(source_root)
        self.runner = runner
        self.journal = WriteAheadJournal(context.journal_path)
        self.executor = TransactionExecutor(self.journal)
        self.allow_unprivileged_system_commit = allow_unprivileged_system_commit
        self.activate_user_services = activate_user_services
        self._staged = {item.artifact_id: item for item in build_report.artifacts}
        self._groups: dict[str, _RollbackGroup] = {}
        self._group_order: list[str] = []
        self._service_groups: set[str] = set()
        self._approved_artifact_before = self._backup_fingerprints()
        self._terminal_integrator: ConfigurationIntegrator | None = None

    # ---- ComponentMutationBackend ---------------------------------------

    def commit_artifacts(self, component: PlannedComponent, actions: tuple[ArtifactAction, ...]) -> HandlerStepResult:
        op_ids: list[str] = []
        try:
            for action in actions:
                staged = self._require_staged(action)
                if self._needs_privilege(action):
                    group = self._commit_privileged(action, staged)
                else:
                    group = self._commit_unprivileged_artifact(action, staged)
                self._register(group)
                op_ids.append(group.group_id)
        except Exception as exc:
            return self._step_failure("artifact commit", exc, tuple(op_ids))
        return HandlerStepResult(True, "validated staged artifacts committed", tuple(op_ids))

    def apply_configuration(self, component: PlannedComponent, actions: tuple[ConfigAction, ...]) -> HandlerStepResult:
        op_ids: list[str] = []
        try:
            if component.id == "terminal":
                integrator = ConfigurationIntegrator(
                    plan=self.plan,
                    source_root=self.source_root,
                    paths=self.paths,
                    journal=self.journal,
                    preimage_dir=self.context.preimage_dir / "terminal",
                    runner=self.runner,
                )
                report = integrator.apply(
                    activate_watcher=self.activate_user_services,
                    rollback_on_failure=True,
                    include_hypr=False,
                )
                if not report.ok:
                    # Phase-11 attempts its own surgical rollback on failure. If
                    # that rollback itself was incomplete, retain the integrator
                    # as an outer transaction rollback group so Phase 16 can try
                    # again rather than losing ownership of partial mutations.
                    if not report.rolled_back:
                        group_id = f"group-terminal-partial-{new_operation_id()}"
                        group = _RollbackGroup(group_id, "terminal_configuration_partial", integrator.rollback)
                        self._register(group)
                        op_ids.append(group_id)
                    raise InstallerError(
                        "Realmheart Terminal configuration integration failed",
                        code="RH_LIVE_TERMINAL_CONFIG_FAILED",
                        stage="configuration",
                        details={"blockers": list(report.blockers)},
                    )
                group_id = f"group-terminal-{new_operation_id()}"
                group = _RollbackGroup(group_id, "terminal_configuration", integrator.rollback)
                self._register(group)
                self._terminal_integrator = integrator
                op_ids.append(group_id)
                return HandlerStepResult(True, "terminal integration applied transactionally", tuple(op_ids), report.warnings)

            for action in actions:
                if not action.will_mutate or action.kind is ConfigActionKind.READ_ONLY:
                    continue
                if action.kind is ConfigActionKind.FULL_TREE_REPLACE:
                    group = self._apply_tree_config(action)
                elif action.kind in {ConfigActionKind.OWNED_FILE, ConfigActionKind.RENDERED_FILE, ConfigActionKind.SHARED_SEED}:
                    group = self._apply_file_config(action)
                    if group is None:
                        continue
                elif action.kind in {ConfigActionKind.MANAGED_BLOCK, ConfigActionKind.GENERATED_STATE}:
                    raise InstallerError(
                        f"configuration action {action.id} requires the terminal-specialized integrator",
                        code="RH_LIVE_CONFIG_SPECIAL_HANDLER_REQUIRED",
                        stage="configuration",
                    )
                else:
                    raise InstallerError(
                        f"unsupported live config action {action.id}: {action.kind.value}",
                        code="RH_LIVE_CONFIG_ACTION_UNSUPPORTED",
                        stage="configuration",
                    )
                self._register(group)
                op_ids.append(group.group_id)
        except Exception as exc:
            return self._step_failure("configuration", exc, tuple(op_ids))
        return HandlerStepResult(True, "configuration actions applied", tuple(op_ids))

    def apply_services(self, component: PlannedComponent, actions: tuple[ServiceAction, ...]) -> HandlerStepResult:
        if not self.activate_user_services:
            return HandlerStepResult(True, "user-service activation suppressed by caller")
        if not actions:
            return HandlerStepResult(True, "no service actions")
        try:
            group = self._apply_service_actions(component, actions)
            self._register(group)
            self._service_groups.add(group.group_id)
            return HandlerStepResult(True, "user service actions applied", (group.group_id,))
        except Exception as exc:
            return self._step_failure("service activation", exc, ())

    def verify_component(self, component: PlannedComponent, checks: tuple[PlannedHealthCheck, ...]) -> HandlerStepResult:
        # Phase 13 performs the authoritative observed-state verification after
        # all components are committed.  The Phase-12 hook deliberately remains
        # side-effect-free here so local install progress does not duplicate or
        # contradict the final verifier.
        return HandlerStepResult(True, "component-local verification deferred to Phase-13 observed-state engine")

    def rollback_component(
        self,
        component: PlannedComponent,
        operation_ids: tuple[str, ...],
        requirements: tuple[RollbackRequirement, ...],
    ) -> HandlerStepResult:
        groups = self._unique_groups(operation_ids)
        errors: list[str] = []
        # Services must be quiesced before restoring their unit/config files.
        for group in reversed(groups):
            if group.group_id in self._service_groups:
                try:
                    group.quiesce()
                except Exception as exc:
                    errors.append(f"{group.group_id} quiesce: {exc}")
        for group in reversed(groups):
            try:
                group.rollback()
            except Exception as exc:
                errors.append(f"{group.group_id}: {exc}")
        for group in groups:
            if group.group_id in self._service_groups:
                try:
                    group.restore()
                except Exception as exc:
                    errors.append(f"{group.group_id} restore service state: {exc}")
        if errors:
            return HandlerStepResult(False, "; ".join(errors), error_code="RH_COMPONENT_ROLLBACK_FAILED")
        return HandlerStepResult(True, "component transaction footprint rolled back")

    # ---- transaction finalization --------------------------------------

    def finalize_keep(self) -> tuple[str, ...]:
        """Remove transaction-old siblings only after the kept state is durable."""
        errors: list[str] = []
        for group_id in self._group_order:
            group = self._groups[group_id]
            try:
                group.finalize()
            except Exception as exc:
                errors.append(f"{group_id}: {exc}")
        return tuple(errors)

    def rollback_all(self) -> tuple[str, ...]:
        """Reverse every registered live mutation in exact reverse order."""
        errors: list[str] = []
        groups = [self._groups[group_id] for group_id in self._group_order]
        for group in reversed(groups):
            if group.group_id in self._service_groups:
                try:
                    group.quiesce()
                except Exception as exc:
                    errors.append(f"{group.group_id} quiesce: {exc}")
        for group in reversed(groups):
            try:
                group.rollback()
            except Exception as exc:
                errors.append(f"{group.group_id}: {exc}")
        for group in groups:
            if group.group_id in self._service_groups:
                try:
                    group.restore()
                except Exception as exc:
                    errors.append(f"{group.group_id} restore service state: {exc}")
        return tuple(errors)


    def restore_permanent_baseline(self, snapshot_dir: Path) -> tuple[str, ...]:
        """Restore the validated permanent baseline after reversing this attempt.

        Snapshot-declared source paths are treated as untrusted input: every path
        must also exist in the current approved backup target set.  A failed
        multi-target baseline restore is reversed best-effort before returning.
        """
        snapshot_dir = Path(snapshot_dir)
        validation = validate_backup_snapshot(snapshot_dir)
        if not validation.valid:
            return ("permanent baseline is invalid: " + "; ".join(validation.errors),)
        try:
            payload = json.loads((snapshot_dir / "manifest.json").read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            return (f"cannot read permanent baseline manifest: {exc}",)
        approved = {target.path: target for backup in self.plan.backup_actions for target in backup.targets}
        applied: list[_RollbackGroup] = []
        try:
            for raw in payload.get("sources", []):
                target_text = raw.get("source")
                if target_text not in approved:
                    raise InstallerError(
                        f"baseline target is not part of the approved Realmheart restore set: {target_text}",
                        code="RH_BASELINE_TARGET_NOT_APPROVED", stage="rollback",
                    )
                target = Path(target_text)
                existed = bool(raw.get("existed"))
                backup_rel = raw.get("backup_relative_path")
                if existed:
                    if not isinstance(backup_rel, str):
                        raise InstallerError("baseline record is missing payload", code="RH_BASELINE_RECORD_INVALID", stage="rollback")
                    source = snapshot_dir / backup_rel
                    if source.is_symlink():
                        raise InstallerError(
                            f"automatic baseline restore of symlink targets is intentionally refused: {target}",
                            code="RH_BASELINE_SYMLINK_MANUAL_ATTENTION", stage="rollback",
                        )
                    artifact_type = "directory" if source.is_dir() else "file"
                    mode = stat.S_IMODE(source.stat(follow_symlinks=False).st_mode)
                    staged = StagedArtifactResult(
                        artifact_id=f"baseline.{raw.get('label', 'item')}",
                        target_path=str(target), staged_path=str(source), artifact_type=artifact_type,
                        required=True, exists=True, type_ok=True, executable_ok=None,
                        mode=f"{mode:04o}", size_bytes=None, sha256=None, fingerprint=fingerprint_path(source), reason=None,
                    )
                    action = ArtifactAction(
                        artifact_id=staged.artifact_id, component_id="baseline-restore", target=str(target),
                        artifact_type=artifact_type, ownership="system" if approved[target_text].privileged else "user",
                        commit_class=self._baseline_commit_class(approved[target_text].privileged), required=True,
                        source=None, privileged=approved[target_text].privileged,
                    )
                    group = self._commit_privileged(action, staged) if approved[target_text].privileged else self._commit_unprivileged_artifact(action, staged)
                else:
                    group = self._remove_for_baseline(target, privileged=approved[target_text].privileged)
                    if group is None:
                        continue
                applied.append(group)
            for group in applied:
                group.finalize()
            return ()
        except Exception as exc:
            errors = [f"baseline restore: {type(exc).__name__}: {exc}"]
            for group in reversed(applied):
                try:
                    group.rollback()
                except Exception as rb_exc:
                    errors.append(f"baseline safety rollback {group.group_id}: {rb_exc}")
            return tuple(errors)

    @staticmethod
    def _baseline_commit_class(privileged: bool):
        from ..planning.models import ArtifactCommitClass
        return ArtifactCommitClass.PRIVILEGED_COMMIT if privileged else ArtifactCommitClass.STAGED_PAYLOAD

    def _remove_for_baseline(self, target: Path, *, privileged: bool) -> _RollbackGroup | None:
        if not (target.exists() or target.is_symlink()):
            return None
        if privileged and not (self.allow_unprivileged_system_commit and self._test_system_target(target)):
            sudo = self.runner.which("sudo")
            if not sudo:
                raise InstallerError("sudo required for privileged baseline removal", code="RH_PRIVILEGED_COMMIT_SUDO_MISSING", stage="rollback")
            tools = self._privileged_tools()
            old = target.parent / f".{target.name}.rh-baseline-old-{self.plan.transaction_id}"
            if old.exists() or old.is_symlink():
                raise InstallerError(f"baseline removal sibling collision: {old}", code="RH_BASELINE_RESTORE_COLLISION", stage="rollback")
            expected = fingerprint_path(target)
            self._sudo(sudo, tools["mv"], "--", str(target), str(old))
            def rollback() -> None:
                if target.exists() or target.is_symlink():
                    raise PreconditionFailedError(str(target), "baseline removal target was recreated")
                self._sudo(sudo, tools["mv"], "--", str(old), str(target))
            def finalize() -> None:
                if old.exists() or old.is_symlink():
                    self._sudo(sudo, tools["rm"], "-rf", "--", str(old))
            return _RollbackGroup(f"group-baseline-remove-{new_operation_id()}", "baseline_remove", rollback, finalize)
        allowed = self._user_boundary(target)
        backup = target.parent / f".{target.name}.rh-baseline-old-{self.plan.transaction_id}"
        op = RemovePathOperation(
            target=target, allowed_root=allowed,
            safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(target), str(backup)),
            backup_path=backup,
        )
        self.executor.execute(op)
        def finalize() -> None:
            _safe_remove_path(backup)
        return _RollbackGroup(f"group-baseline-remove-{op.operation_id}", "baseline_remove", lambda: self.executor.rollback(op), finalize)

    # ---- unprivileged artifact/config primitives -----------------------

    def _commit_unprivileged_artifact(self, action: ArtifactAction, staged: StagedArtifactResult) -> _RollbackGroup:
        target = Path(action.target)
        source = Path(staged.staged_path)
        expected = self._approved_artifact_before.get(str(target))
        self._check_expected_before(target, expected)
        if action.artifact_type == "directory":
            # Directory artifacts may be the first payload placed below a fresh
            # prefix (for example <prefix>/share/realmheart/assets). Create only
            # the missing parent chain through transaction-aware operations so a
            # rollback can remove directories that did not exist before this
            # install. Requiring the parent to pre-exist made a fresh install
            # impossible even though the target itself was perfectly safe.
            created_dirs = self._ensure_user_directories(target.parent, self._user_boundary(target))
            try:
                stage = prepare_full_tree_stage(
                    release_tree=source,
                    target=target,
                    transaction_id=self.plan.transaction_id,
                    preserve_relative_paths=(),
                )
                if expected is not None and stage.active_precondition_fingerprint != expected:
                    shutil.rmtree(stage.staging, ignore_errors=True)
                    raise PreconditionFailedError(str(target), "artifact target changed since approved backup/plan")
                swap = FullTreeSwap(stage, self.journal)
                try:
                    swap.execute()
                except Exception:
                    if swap.operations:
                        swap.rollback()
                    raise
            except Exception:
                for created in reversed(created_dirs):
                    created.rollback()
                raise

            group_id = f"group-artifact-tree-{action.artifact_id}-{new_operation_id()}"

            def rollback() -> None:
                swap.rollback()
                # A reversed fresh swap puts the transaction-owned payload back
                # at the sibling staging path. Explicit rollback is complete
                # only after that staging tree is removed; otherwise parent
                # directory rollback correctly refuses because it is non-empty.
                _safe_remove_path(stage.staging)
                for created in reversed(created_dirs):
                    created.rollback()

            return _RollbackGroup(
                group_id,
                "artifact_tree",
                rollback,
                finalize_fn=lambda: _safe_remove_path(stage.old),
            )

        content = source.read_bytes()
        mode = stat.S_IMODE(source.stat(follow_symlinks=False).st_mode)
        groups = self._ensure_user_directories(target.parent, self._user_boundary(target))
        op_id = new_operation_id()
        preimage = self.context.preimage_dir / "artifacts" / f"{op_id}.preimage"
        precondition = MutationPrecondition(
            FINGERPRINT_PRECONDITION,
            expected_fingerprint=expected if expected is not None else fingerprint_path(target),
            expected_exists=(target.exists() or target.is_symlink()) if expected is None else None,
        )
        op = WriteFileOperation(
            target=target,
            allowed_root=self._user_boundary(target),
            safety=OperationSafety(Reversibility.EXACT, precondition, str(preimage)),
            operation_id=op_id,
            content=content,
            mode=mode,
            preimage_path=preimage,
        )
        self.executor.execute(op)
        write_group = _RollbackGroup(f"group-artifact-file-{action.artifact_id}-{op_id}", "artifact_file", lambda: self.executor.rollback(op))
        if groups:
            # Parent creation belongs to the same logical rollback group and is
            # reversed only after the file itself.
            def rollback() -> None:
                self.executor.rollback(op)
                for created in reversed(groups):
                    created.rollback()
            return _RollbackGroup(write_group.group_id, "artifact_file", rollback)
        return write_group

    def _apply_tree_config(self, action: ConfigAction) -> _RollbackGroup:
        target = Path(action.target)
        source = Path(action.source or "")
        if fingerprint_path(target) != action.precondition_fingerprint:
            raise PreconditionFailedError(action.target, "target changed since InstallationPlan approval")
        self._ensure_directory_unjournaled_parent(target.parent)
        preserve = tuple(Path(item).relative_to(target).as_posix() for item in action.preserve)
        stage = prepare_full_tree_stage(
            release_tree=source,
            target=target,
            transaction_id=self.plan.transaction_id,
            preserve_relative_paths=preserve,
        )
        if stage.active_precondition_fingerprint != action.precondition_fingerprint:
            shutil.rmtree(stage.staging, ignore_errors=True)
            raise PreconditionFailedError(action.target, "target changed during full-tree staging")
        swap = FullTreeSwap(stage, self.journal)
        try:
            swap.execute()
        except Exception:
            if swap.operations:
                swap.rollback()
            raise
        group_id = f"group-config-tree-{new_operation_id()}"

        def rollback() -> None:
            swap.rollback()
            _safe_remove_path(stage.staging)

        return _RollbackGroup(group_id, "config_tree", rollback, finalize_fn=lambda: _safe_remove_path(stage.old))

    def _apply_file_config(self, action: ConfigAction) -> _RollbackGroup | None:
        target = Path(action.target)
        current = fingerprint_path(target)
        if current != action.precondition_fingerprint:
            raise PreconditionFailedError(action.target, "target changed since InstallationPlan approval")
        if action.kind is ConfigActionKind.SHARED_SEED and (target.exists() or target.is_symlink()):
            return None
        if action.source is None and action.kind is ConfigActionKind.SHARED_SEED:
            raise InstallerError("shared seed has no source", code="RH_LIVE_CONFIG_SOURCE_MISSING", stage="configuration")
        content, mode = render_action_content(action, source_root=self.source_root)
        desired = fingerprint_regular_bytes(content, mode)
        if current == desired:
            return None
        created_dirs = self._ensure_user_directories(target.parent, self._user_boundary(target))
        op_id = new_operation_id()
        preimage = self.context.preimage_dir / "config" / f"{op_id}.preimage"
        op = WriteFileOperation(
            target=target,
            allowed_root=self._user_boundary(target),
            safety=OperationSafety(
                action.reversibility if action.reversibility is not Reversibility.NONE else Reversibility.EXACT,
                MutationPrecondition(FINGERPRINT_PRECONDITION, action.precondition_fingerprint, target.exists() or target.is_symlink()),
                str(preimage),
            ),
            operation_id=op_id,
            content=content,
            mode=mode,
            preimage_path=preimage,
        )
        self.executor.execute(op)

        def rollback() -> None:
            self.executor.rollback(op)
            for group in reversed(created_dirs):
                group.rollback()

        return _RollbackGroup(f"group-config-file-{action.id}-{op_id}", "config_file", rollback)

    # ---- privileged artifacts ------------------------------------------

    def _commit_privileged(self, action: ArtifactAction, staged: StagedArtifactResult) -> _RollbackGroup:
        target = Path(action.target)
        if self.allow_unprivileged_system_commit and self._test_system_target(target):
            return self._commit_unprivileged_artifact(action, staged)
        source = Path(staged.staged_path)
        expected = self._approved_artifact_before.get(str(target))
        self._check_expected_before(target, expected)
        sudo = self.runner.which("sudo")
        if not sudo:
            raise InstallerError(
                f"narrow privilege escalation is required to commit {target}",
                code="RH_PRIVILEGED_COMMIT_SUDO_MISSING",
                stage="live_commit",
            )
        commands = self._privileged_tools()
        token = _safe_token(action.artifact_id)
        candidate = target.parent / f".{target.name}.rh-new-{self.plan.transaction_id}-{token}"
        old = target.parent / f".{target.name}.rh-old-{self.plan.transaction_id}-{token}"
        if candidate.exists() or candidate.is_symlink() or old.exists() or old.is_symlink():
            raise InstallerError(
                f"privileged artifact sibling collision at {candidate} or {old}",
                code="RH_PRIVILEGED_COMMIT_COLLISION",
                stage="live_commit",
            )
        op_id = new_operation_id()
        before_exists = target.exists() or target.is_symlink()
        privileged_boundary, missing_parent_dirs = self._inspect_privileged_parent_chain(target)
        self.journal.append(
            operation_id=op_id,
            state=OperationState.INTENT,
            kind="privileged_replace",
            target=str(target),
            data={
                "reversibility": Reversibility.EXACT.value,
                "before_fingerprint": expected or fingerprint_path(target),
                "candidate": str(candidate),
                "old": str(old),
                "source": str(source),
                "artifact_id": action.artifact_id,
                "existed_before": before_exists,
                "privileged_boundary": str(privileged_boundary),
                "created_parent_dirs": [str(item) for item in missing_parent_dirs],
            },
        )
        self.journal.append(operation_id=op_id, state=OperationState.STARTED, kind="privileged_replace", target=str(target))
        expected_after: str | None = None
        try:
            self._create_privileged_parent_dirs(sudo, commands, privileged_boundary, missing_parent_dirs)
            self._sudo(sudo, commands["cp"], "-a", "--", str(source), str(candidate))
            self._sudo(sudo, commands["chown"], "-R", "0:0", "--", str(candidate))
            mode = stat.S_IMODE(source.stat(follow_symlinks=False).st_mode)
            self._sudo(sudo, commands["chmod"], f"{mode:o}", "--", str(candidate))
            if before_exists:
                self._sudo(sudo, commands["mv"], "--", str(target), str(old))
            self._sudo(sudo, commands["mv"], "--", str(candidate), str(target))
            expected_after = fingerprint_path(target)
            if staged.fingerprint is not None and expected_after != staged.fingerprint:
                # Root ownership is excluded from fingerprints, so staged/live
                # content+mode should remain identical.
                raise InstallerError(
                    f"live privileged artifact identity differs from validated stage: {action.artifact_id}",
                    code="RH_PRIVILEGED_COMMIT_IDENTITY_MISMATCH",
                    stage="live_commit",
                )
        except Exception as exc:
            self.journal.append(
                operation_id=op_id,
                state=OperationState.FAILED,
                kind="privileged_replace",
                target=str(target),
                data={"error_type": type(exc).__name__, "error": str(exc)},
            )
            try:
                self._rollback_privileged_partial(
                    sudo, commands, target, candidate, old, before_exists, expected_after,
                    missing_parent_dirs,
                )
                self.journal.append(operation_id=op_id, state=OperationState.ROLLED_BACK, kind="privileged_replace", target=str(target), data={"partial_failure": True})
            except Exception as rb_exc:
                self.journal.append(operation_id=op_id, state=OperationState.ROLLBACK_FAILED, kind="privileged_replace", target=str(target), data={"error": str(rb_exc)})
            raise
        self.journal.append(
            operation_id=op_id,
            state=OperationState.COMPLETED,
            kind="privileged_replace",
            target=str(target),
            data={"after_fingerprint": expected_after},
        )

        def rollback() -> None:
            self.journal.append(operation_id=op_id, state=OperationState.ROLLBACK_STARTED, kind="privileged_replace", target=str(target))
            current = fingerprint_path(target)
            if expected_after is not None and current != expected_after:
                raise PreconditionFailedError(str(target), "privileged artifact changed after Realmheart commit; refusing destructive rollback")
            self._rollback_privileged_partial(
                sudo, commands, target, candidate, old, before_exists, expected_after,
                missing_parent_dirs,
            )
            self.journal.append(operation_id=op_id, state=OperationState.ROLLED_BACK, kind="privileged_replace", target=str(target))

        def finalize() -> None:
            if old.exists() or old.is_symlink():
                self._sudo(sudo, commands["rm"], "-rf", "--", str(old))
            if candidate.exists() or candidate.is_symlink():
                self._sudo(sudo, commands["rm"], "-rf", "--", str(candidate))

        return _RollbackGroup(f"group-privileged-{action.artifact_id}-{op_id}", "privileged_artifact", rollback, finalize)

    def _rollback_privileged_partial(
        self,
        sudo: str,
        tools: dict[str, str],
        target: Path,
        candidate: Path,
        old: Path,
        existed_before: bool,
        expected_after: str | None,
        created_parent_dirs: tuple[Path, ...],
    ) -> None:
        scratch = target.parent / f".{target.name}.rh-rollback-{self.plan.transaction_id}"
        if scratch.exists() or scratch.is_symlink():
            raise InstallerError(f"rollback sibling collision: {scratch}", code="RH_PRIVILEGED_ROLLBACK_COLLISION", stage="rollback")
        if old.exists() or old.is_symlink():
            if target.exists() or target.is_symlink():
                self._sudo(sudo, tools["mv"], "--", str(target), str(scratch))
            self._sudo(sudo, tools["mv"], "--", str(old), str(target))
            if scratch.exists() or scratch.is_symlink():
                self._sudo(sudo, tools["rm"], "-rf", "--", str(scratch))
        elif not existed_before and (target.exists() or target.is_symlink()):
            self._sudo(sudo, tools["rm"], "-rf", "--", str(target))
        if candidate.exists() or candidate.is_symlink():
            self._sudo(sudo, tools["rm"], "-rf", "--", str(candidate))
        for directory in reversed(created_parent_dirs):
            # rmdir is deliberately used instead of rm -rf. If anything else
            # appeared in a transaction-created parent, rollback must stop and
            # report the conflict rather than deleting unrelated state.
            self._sudo(sudo, tools["rmdir"], "--", str(directory))

    # ---- services -------------------------------------------------------

    def _apply_service_actions(self, component: PlannedComponent, actions: tuple[ServiceAction, ...]) -> _RollbackGroup:
        systemctl = self._systemctl()
        before: dict[str, _UnitState] = {}
        mutated_services: list[str] = []
        op_id = new_operation_id()
        for action in actions:
            if action.action is ServiceActionKind.DAEMON_RELOAD or action.action is ServiceActionKind.INSTALL_ONLY:
                continue
            before[action.service] = self._unit_state(systemctl, action.service)
        self.journal.append(
            operation_id=op_id,
            state=OperationState.INTENT,
            kind="systemd_user_services",
            target=component.id,
            data={"before": {key: {"enabled": val.enabled, "active": val.active} for key, val in before.items()}},
        )
        self.journal.append(operation_id=op_id, state=OperationState.STARTED, kind="systemd_user_services", target=component.id)
        try:
            for action in actions:
                if action.action is ServiceActionKind.DAEMON_RELOAD:
                    self._run_required((systemctl, "--user", "daemon-reload"), "systemd user daemon-reload failed")
                elif action.action is ServiceActionKind.INSTALL_ONLY:
                    continue
                elif action.action is ServiceActionKind.ENABLE_START:
                    self._run_required((systemctl, "--user", "enable", "--now", action.service), f"failed enabling/starting {action.service}", interactive=True)
                    mutated_services.append(action.service)
                elif action.action is ServiceActionKind.ENABLE_ONLY:
                    self._run_required((systemctl, "--user", "enable", action.service), f"failed enabling {action.service}", interactive=True)
                    mutated_services.append(action.service)
                else:
                    raise InstallerError(f"unsupported service action {action.action.value}", code="RH_SERVICE_ACTION_UNSUPPORTED", stage="services")
        except Exception as exc:
            self.journal.append(operation_id=op_id, state=OperationState.FAILED, kind="systemd_user_services", target=component.id, data={"error": str(exc)})
            self._restore_unit_states(systemctl, before)
            raise
        self.journal.append(operation_id=op_id, state=OperationState.COMPLETED, kind="systemd_user_services", target=component.id)

        def quiesce() -> None:
            for service in reversed(mutated_services):
                self.runner.run((systemctl, "--user", "stop", service), timeout=10.0)

        def rollback() -> None:
            self.journal.append(operation_id=op_id, state=OperationState.ROLLBACK_STARTED, kind="systemd_user_services", target=component.id)
            # Actual prior-state restoration is deferred until unit files have
            # been restored by the rest of the component rollback.
            self.journal.append(operation_id=op_id, state=OperationState.ROLLED_BACK, kind="systemd_user_services", target=component.id, data={"files_pending": True})

        def restore() -> None:
            self._run_required((systemctl, "--user", "daemon-reload"), "systemd user daemon-reload failed during rollback")
            self._restore_unit_states(systemctl, before)

        return _RollbackGroup(f"group-service-{component.id}-{op_id}", "service", rollback, quiesce_fn=quiesce, restore_fn=restore)

    def _unit_state(self, systemctl: str, service: str) -> _UnitState:
        enabled = self.runner.run((systemctl, "--user", "is-enabled", service), timeout=4.0)
        active = self.runner.run((systemctl, "--user", "is-active", service), timeout=4.0)
        return _UnitState(enabled.ok and enabled.stdout.strip() not in {"disabled", "masked", "not-found"}, active.ok and active.stdout.strip() == "active")

    def _restore_unit_states(self, systemctl: str, states: dict[str, _UnitState]) -> None:
        for service, state in states.items():
            if state.enabled:
                self._run_required((systemctl, "--user", "enable", service), f"failed restoring enabled state for {service}", interactive=True)
            else:
                self.runner.run((systemctl, "--user", "disable", service), timeout=10.0, interactive=True)
            if state.active:
                self._run_required((systemctl, "--user", "start", service), f"failed restoring active state for {service}", interactive=True)
            else:
                self.runner.run((systemctl, "--user", "stop", service), timeout=10.0, interactive=True)

    # ---- helpers --------------------------------------------------------

    def _require_staged(self, action: ArtifactAction) -> StagedArtifactResult:
        staged = self._staged.get(action.artifact_id)
        if staged is None or not staged.ok:
            raise InstallerError(
                f"validated stage is missing required artifact {action.artifact_id}",
                code="RH_LIVE_STAGE_ARTIFACT_MISSING",
                stage="live_commit",
            )
        if staged.target_path != action.target:
            raise InstallerError(
                f"staged artifact target drift for {action.artifact_id}",
                code="RH_LIVE_STAGE_TARGET_MISMATCH",
                stage="live_commit",
            )
        return staged

    def _backup_fingerprints(self) -> dict[str, str]:
        values: dict[str, str] = {}
        for backup in self.plan.backup_actions:
            for target in backup.targets:
                values[target.path] = target.fingerprint
        return values

    @staticmethod
    def _check_expected_before(target: Path, expected: str | None) -> None:
        if expected is not None and fingerprint_path(target) != expected:
            raise PreconditionFailedError(str(target), "artifact target changed since backup/planning")

    def _ensure_user_directories(self, directory: Path, allowed_root: Path) -> list[_RollbackGroup]:
        directory = Path(directory)
        missing: list[Path] = []
        current = directory
        while not (current.exists() or current.is_symlink()):
            missing.append(current)
            if current == allowed_root:
                break
            current = current.parent
        if current.is_symlink():
            raise InstallerError(f"refusing path beneath symlinked directory {current}", code="RH_LIVE_SYMLINK_PARENT", stage="live_commit")
        groups: list[_RollbackGroup] = []
        for path in reversed(missing):
            if path == allowed_root:
                raise InstallerError(f"live safety root does not exist: {allowed_root}", code="RH_LIVE_ROOT_MISSING", stage="live_commit")
            op = CreateDirectoryOperation(
                target=path,
                allowed_root=allowed_root,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(path), None),
                mode=0o700,
            )
            self.executor.execute(op)
            group = _RollbackGroup(f"group-dir-{op.operation_id}", "directory", lambda op=op: self.executor.rollback(op))
            groups.append(group)
        return groups

    @staticmethod
    def _ensure_directory_unjournaled_parent(directory: Path) -> None:
        if not directory.is_dir() or directory.is_symlink():
            raise InstallerError(f"required target parent is missing or unsafe: {directory}", code="RH_LIVE_PARENT_UNSAFE", stage="live_commit")

    def _user_boundary(self, target: Path) -> Path:
        target_abs = Path(os.path.abspath(target))
        user_roots = (self.paths.config_home, self.paths.state_home, self.paths.data_home)
        for root in user_roots:
            root_abs = Path(os.path.abspath(root))
            try:
                if Path(os.path.commonpath([target_abs, root_abs])) == root_abs:
                    return root_abs.parent
            except ValueError:
                pass
        local_bin = Path(os.path.abspath(self.paths.home / ".local/bin"))
        try:
            if Path(os.path.commonpath([target_abs, local_bin])) == local_bin:
                # ~/.local may legitimately not exist on a fresh account. The
                # immutable safety boundary is HOME, allowing the transaction
                # kernel to create .local/bin itself and remove it on rollback.
                return Path(os.path.abspath(self.paths.home))
        except ValueError:
            pass
        # Fake-prefix tests use an explicitly non-production prefix which is
        # handled as unprivileged system commit only when opted in.
        prefix = Path(self.plan.layout.prefix)
        sysconf = Path(self.plan.layout.sysconf)
        for root in (prefix, sysconf):
            try:
                if Path(os.path.commonpath([target_abs, Path(os.path.abspath(root))])) == Path(os.path.abspath(root)):
                    return root.parent
            except ValueError:
                pass
        raise InstallerError(f"target outside approved live roots: {target}", code="RH_LIVE_TARGET_OUTSIDE_SCOPE", stage="live_commit")

    def _needs_privilege(self, action: ArtifactAction) -> bool:
        return bool(action.privileged)

    def _test_system_target(self, target: Path) -> bool:
        # Production roots are never silently downgraded to direct writes.
        production = (Path("/usr/local"), Path("/etc"))
        target_abs = Path(os.path.abspath(target))
        for root in production:
            try:
                if Path(os.path.commonpath([target_abs, root])) == root:
                    return False
            except ValueError:
                pass
        return True

    def _inspect_privileged_parent_chain(self, target: Path) -> tuple[Path, tuple[Path, ...]]:
        """Validate the existing trusted chain and return missing child dirs.

        Only production privileged roots are accepted here.  Existing parents
        must be real root-owned directories with no group/world write bit.  The
        returned missing directories are ordered from the trusted root outward
        so they can be created without ``mkdir -p`` races.
        """

        target_abs = Path(os.path.abspath(target))
        boundaries = (Path("/usr/local"), Path("/etc"))
        boundary = None
        for candidate in boundaries:
            try:
                if Path(os.path.commonpath([target_abs, candidate])) == candidate:
                    boundary = candidate
                    break
            except ValueError:
                continue
        if boundary is None:
            raise InstallerError(
                f"privileged target is outside approved production roots: {target}",
                code="RH_PRIVILEGED_TARGET_OUTSIDE_SCOPE", stage="live_commit",
            )

        chain: list[Path] = [boundary]
        relative = target_abs.parent.relative_to(boundary)
        current = boundary
        for part in relative.parts:
            current = current / part
            chain.append(current)

        missing: list[Path] = []
        saw_missing = False
        for path in chain:
            if path.exists() or path.is_symlink():
                if saw_missing:
                    raise InstallerError(
                        f"privileged parent appeared below a missing ancestor: {path}",
                        code="RH_PRIVILEGED_PARENT_RACE", stage="live_commit",
                    )
                self._assert_secure_privileged_directory(path, expected_uid=0)
            else:
                saw_missing = True
                if path == boundary:
                    raise InstallerError(
                        f"trusted privileged boundary is missing: {boundary}",
                        code="RH_PRIVILEGED_BOUNDARY_MISSING", stage="live_commit",
                    )
                missing.append(path)
        return boundary, tuple(missing)

    @staticmethod
    def _assert_secure_privileged_directory(path: Path, *, expected_uid: int) -> None:
        try:
            st = path.lstat()
        except OSError as exc:
            raise InstallerError(
                f"cannot inspect privileged parent {path}: {exc}",
                code="RH_PRIVILEGED_PARENT_INSPECTION_FAILED", stage="live_commit",
            ) from exc
        if stat.S_ISLNK(st.st_mode) or not stat.S_ISDIR(st.st_mode):
            raise InstallerError(
                f"privileged parent is not a real directory: {path}",
                code="RH_PRIVILEGED_PARENT_UNSAFE", stage="live_commit",
            )
        if st.st_uid != expected_uid or stat.S_IMODE(st.st_mode) & 0o022:
            raise InstallerError(
                f"privileged parent is not securely owned/mode-protected: {path}",
                code="RH_PRIVILEGED_PARENT_UNSAFE", stage="live_commit",
                details={"uid": st.st_uid, "mode": f"{stat.S_IMODE(st.st_mode):04o}"},
            )

    def _create_privileged_parent_dirs(
        self,
        sudo: str,
        tools: dict[str, str],
        boundary: Path,
        missing: tuple[Path, ...],
    ) -> None:
        self._assert_secure_privileged_directory(boundary, expected_uid=0)
        for directory in missing:
            # No -p: if another process races us and creates this path, mkdir
            # fails and we refuse to chown/chmod something we did not create.
            self._sudo(sudo, tools["mkdir"], "--", str(directory))
            self._sudo(sudo, tools["chown"], "0:0", "--", str(directory))
            self._sudo(sudo, tools["chmod"], "755", "--", str(directory))
            self._assert_secure_privileged_directory(directory, expected_uid=0)

    def _privileged_tools(self) -> dict[str, str]:
        result = {}
        for name in ("mkdir", "rmdir", "cp", "chown", "chmod", "mv", "rm"):
            path = self.runner.which(name)
            if not path:
                raise InstallerError(f"required privileged file tool is missing: {name}", code="RH_PRIVILEGED_TOOL_MISSING", stage="live_commit")
            result[name] = path
        return result

    def _sudo(self, sudo: str, executable: str, *args: str) -> None:
        result = self.runner.run((sudo, executable, *args), timeout=120.0, interactive=True)
        if not result.ok:
            raise InstallerError(
                f"privileged command failed: {Path(executable).name}",
                code="RH_PRIVILEGED_COMMAND_FAILED",
                stage="live_commit",
                details={"command": Path(executable).name, "returncode": result.returncode},
            )

    def _systemctl(self) -> str:
        for capability in self.plan.environment.capabilities:
            if capability.capability_id == "runtime.systemctl" and capability.executable:
                return capability.executable
        return self.runner.which("systemctl") or "systemctl"

    def _run_required(self, argv: tuple[str, ...], message: str, *, interactive: bool = False) -> None:
        result = self.runner.run(argv, timeout=30.0, interactive=interactive)
        if not result.ok:
            raise InstallerError(message, code="RH_SERVICE_COMMAND_FAILED", stage="services", details={"returncode": result.returncode})

    def _register(self, group: _RollbackGroup) -> None:
        if group.group_id in self._groups:
            return
        self._groups[group.group_id] = group
        self._group_order.append(group.group_id)

    def _unique_groups(self, ids: tuple[str, ...]) -> list[_RollbackGroup]:
        seen: set[str] = set()
        groups: list[_RollbackGroup] = []
        for group_id in ids:
            if group_id in seen:
                continue
            seen.add(group_id)
            group = self._groups.get(group_id)
            if group is not None:
                groups.append(group)
        return groups

    @staticmethod
    def _step_failure(step: str, exc: Exception, operation_ids: tuple[str, ...]) -> HandlerStepResult:
        if isinstance(exc, InstallerError):
            return HandlerStepResult(False, exc.message, operation_ids, error_code=exc.code)
        return HandlerStepResult(False, f"{step} failed: {type(exc).__name__}: {exc}", operation_ids, error_code="RH_LIVE_COMPONENT_STEP_FAILED")


def _safe_remove_path(path: Path) -> None:
    if not (path.exists() or path.is_symlink()):
        return
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def _safe_token(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "-", value)[:48]
