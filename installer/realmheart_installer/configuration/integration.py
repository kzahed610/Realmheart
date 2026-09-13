"""Transaction-aware Realmheart configuration and terminal integration.

Phase 11 deliberately exposes this as a reusable executor rather than wiring a
live top-level install command.  Phase 12 component handlers consume it after
backup/build prerequisites are satisfied.  Tests exercise the exact same code
against fake homes and custom XDG roots.
"""

from __future__ import annotations

import os
import shutil
import stat
import uuid
from dataclasses import dataclass
from pathlib import Path

from ..context import XdgPaths
from ..durability import fsync_directory
from ..environment.command import CommandRunner
from ..errors import ConfigurationIntegrationError, InstallerError, PreconditionFailedError
from ..filesystem.compare import fingerprint_path, fingerprint_regular_bytes
from ..filesystem.managed_block import ManagedBlock, plan_ensure_managed_block
from ..filesystem.staging import FullTreeStage, FullTreeSwap, prepare_full_tree_stage
from ..models import OperationSafety, OperationState, Reversibility
from ..planning.models import ConfigAction, ConfigActionKind, InstallationPlan
from ..transaction.journal import WriteAheadJournal
from ..transaction.operations import (
    CreateDirectoryOperation,
    TransactionExecutor,
    TransactionOperation,
    WriteFileOperation,
    new_operation_id,
)
from ..transaction.preconditions import capture_path_precondition
from .models import (
    ConfigurationIntegrationReport,
    GeneratedArtifactResult,
    MutationResult,
    ServiceIntegrationResult,
    UnitState,
    VerificationResult,
)
from .terminal import (
    KITTY_BEGIN,
    KITTY_END,
    generated_results,
    kitty_managed_body,
    render_action_content,
    validate_terminal_state,
)


@dataclass(frozen=True)
class _GeneratedPreimage:
    path: Path
    existed: bool
    content: bytes | None
    mode: int | None
    before_fingerprint: str


class ConfigurationIntegrator:
    """Apply Phase-11 Hypr + terminal state through the transaction kernel.

    This class intentionally does not install `/usr/local` payloads or privileged
    PAM/auth files.  It owns only the user-configuration scope defined by Phase
    11.  A caller may disable service activation for offline/fake-root tests.
    """

    def __init__(
        self,
        *,
        plan: InstallationPlan,
        source_root: Path,
        paths: XdgPaths,
        journal: WriteAheadJournal,
        preimage_dir: Path,
        runner: CommandRunner,
    ) -> None:
        self.plan = plan
        self.source_root = Path(source_root)
        self.paths = paths
        self.journal = journal
        self.preimage_dir = Path(preimage_dir)
        self.runner = runner
        self.executor = TransactionExecutor(journal)
        self._operations: list[TransactionOperation] = []
        self._hypr_stage: FullTreeStage | None = None
        self._hypr_swap: FullTreeSwap | None = None
        self._generated_preimages: dict[str, _GeneratedPreimage] = {}
        self._generated_after: dict[str, str] = {}
        self._generation_operation_id: str | None = None
        self._service_before: UnitState | None = None
        self._service_operation_id: str | None = None
        self._service_mutated = False
        self._warnings: list[str] = []
        self._last_report: ConfigurationIntegrationReport | None = None

    def apply(
        self,
        *,
        activate_watcher: bool = True,
        rollback_on_failure: bool = True,
        include_hypr: bool = True,
    ) -> ConfigurationIntegrationReport:
        mutations: list[MutationResult] = []
        verification: list[VerificationResult] = []
        blockers: list[str] = []
        rolled_back = False
        service_result = ServiceIntegrationResult(False, None, None, (), True, "watcher activation suppressed")

        try:
            if not self.plan.ready:
                raise ConfigurationIntegrationError(
                    "Configuration integration refuses a blocked InstallationPlan",
                    code="RH_CONFIG_PLAN_BLOCKED",
                )
            self.preimage_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
            actions = self._phase11_actions(include_hypr=include_hypr)
            self._validate_action_contract(actions, include_hypr=include_hypr)
            self._verify_approved_preconditions(actions)

            # config_home must exist before same-filesystem Hypr staging can be
            # prepared. Directory creation itself is journaled and rollbackable.
            self._ensure_directory(self.paths.config_home, allowed_root=self.paths.config_home.parent)

            hypr = self._action(actions, "config.hypr.takeover") if include_hypr else None
            if hypr is not None:
                self._hypr_stage = prepare_full_tree_stage(
                    release_tree=Path(hypr.source or ""),
                    target=Path(hypr.target),
                    transaction_id=self.plan.transaction_id,
                    preserve_relative_paths=("custom",),
                )
                if self._hypr_stage.active_precondition_fingerprint != hypr.precondition_fingerprint:
                    raise PreconditionFailedError(hypr.target, "Hypr tree changed after the approved plan was built")

            fish_action = self._action(actions, "config.fish.personal")
            fish_before = fingerprint_path(Path(fish_action.target))

            # Realmheart-owned terminal files first; the shared Kitty block is
            # inserted only after its drop-in exists.
            for action in actions:
                if action.kind not in {ConfigActionKind.OWNED_FILE, ConfigActionKind.RENDERED_FILE}:
                    continue
                if action.component_id != "terminal":
                    continue
                mutations.append(self._apply_owned_file(action))

            kitty = self._action(actions, "config.kitty.managed-block")
            mutations.append(self._apply_kitty_block(kitty))

            # Hypr is activated only after the terminal file set is safely in
            # place, minimizing the mixed-state window.  Phase-16 live component
            # execution may already have committed the Hypr component, in which
            # case terminal integration deliberately excludes this action.
            if self._hypr_stage is not None and hypr is not None:
                self._hypr_swap = FullTreeSwap(self._hypr_stage, self.journal)
                self._hypr_swap.execute()
                mutations.append(MutationResult(
                    action_id=hypr.id,
                    target=hypr.target,
                    changed=True,
                    before_fingerprint=hypr.precondition_fingerprint,
                    after_fingerprint=fingerprint_path(Path(hypr.target)),
                    operation_ids=tuple(op.operation_id for op in self._hypr_swap.operations),
                    detail="atomic full-tree swap; custom/ preservation island overlaid",
                ))

            generated_paths = self._generated_paths()
            self._capture_generated_preimages(generated_paths)
            self._run_generator(generated_paths)
            generated = generated_results(generated_paths)
            self._generated_after = {item.artifact_id: item.fingerprint for item in generated}

            verification.extend(validate_terminal_state(
                home=self.paths.home,
                config_home=self.paths.config_home,
                state_home=self.paths.state_home,
                generated_paths=generated_paths,
                runner=self.runner,
                pycache_root=self.preimage_dir / "pycache",
            ))

            fish_after = fingerprint_path(Path(fish_action.target))
            verification.append(VerificationResult(
                "terminal.fish.config-untouched",
                fish_before == fish_after,
                "byte/fingerprint identity preserved" if fish_before == fish_after else "personal config.fish changed during integration",
            ))

            if any(not item.ok for item in verification):
                failed = ", ".join(item.id for item in verification if not item.ok)
                raise ConfigurationIntegrationError(
                    f"Terminal verification failed: {failed}",
                    code="RH_TERMINAL_VERIFY_FAILED",
                    details={"failed_checks": [item.id for item in verification if not item.ok]},
                )

            if activate_watcher:
                service_result = self._activate_terminal_watcher()
                if not service_result.ok:
                    raise ConfigurationIntegrationError(
                        service_result.detail,
                        code="RH_TERMINAL_WATCHER_FAILED",
                    )
                # The oneshot service regenerates state. Record the final
                # transaction-owned fingerprints for guarded rollback.
                generated = generated_results(generated_paths)
                self._generated_after = {item.artifact_id: item.fingerprint for item in generated}
            else:
                service_result = ServiceIntegrationResult(
                    False, None, None, (), True, "watcher activation intentionally suppressed by caller"
                )

            report = ConfigurationIntegrationReport(
                transaction_id=self.plan.transaction_id,
                mutations=tuple(mutations),
                generated_artifacts=tuple(generated),
                verification=tuple(verification),
                service=service_result,
                warnings=tuple(self._warnings),
                blockers=(),
                rolled_back=False,
            )
            self._last_report = report
            return report
        except Exception as exc:
            if isinstance(exc, InstallerError):
                blockers.append(f"{exc.code}: {exc.message}")
            else:
                blockers.append(f"RH_CONFIG_UNEXPECTED: {type(exc).__name__}: {exc}")
            if rollback_on_failure and self._has_live_mutation():
                try:
                    self.rollback()
                    rolled_back = True
                except Exception as rollback_exc:
                    blockers.append(f"RH_CONFIG_ROLLBACK_FAILED: {type(rollback_exc).__name__}: {rollback_exc}")
            generated = generated_results(self._generated_paths()) if self._generated_paths() else ()
            report = ConfigurationIntegrationReport(
                transaction_id=self.plan.transaction_id,
                mutations=tuple(mutations),
                generated_artifacts=tuple(generated),
                verification=tuple(verification),
                service=service_result,
                warnings=tuple(self._warnings),
                blockers=tuple(blockers),
                rolled_back=rolled_back,
            )
            self._last_report = report
            return report

    def rollback(self) -> None:
        """Surgically reverse the mutations owned by this integration run."""

        # Stop/disable transaction-introduced watcher state before restoring its
        # unit files. This is best-effort but journaled.
        self._quiesce_service_for_rollback()

        if self._hypr_swap is not None and self._hypr_swap.operations:
            self._hypr_swap.rollback()

        for operation in reversed(self._operations):
            self.executor.rollback(operation)

        self._restore_generated_state()
        self._cleanup_unactivated_hypr_stage()
        self._restore_service_after_files()

    def _phase11_actions(self, *, include_hypr: bool = True) -> tuple[ConfigAction, ...]:
        selected = []
        for action in self.plan.config_actions:
            if action.id in {
                "config.kitty.managed-block",
                "config.fish.personal",
                "config.terminal.generated-state",
            }:
                selected.append(action)
                continue
            if include_hypr and action.id == "config.hypr.takeover":
                selected.append(action)
                continue
            if action.id.startswith("config.artifact.terminal."):
                selected.append(action)
        return tuple(selected)

    @staticmethod
    def _action(actions: tuple[ConfigAction, ...], action_id: str) -> ConfigAction:
        for action in actions:
            if action.id == action_id:
                return action
        raise ConfigurationIntegrationError(
            f"Authoritative plan is missing Phase-11 action {action_id}",
            code="RH_CONFIG_PLAN_CONTRACT_MISSING",
            details={"action_id": action_id},
        )

    def _validate_action_contract(self, actions: tuple[ConfigAction, ...], *, include_hypr: bool = True) -> None:
        required = {
            "config.kitty.managed-block",
            "config.fish.personal",
            "config.artifact.terminal.kitty-dropin",
            "config.artifact.terminal.fish-theme",
            "config.artifact.terminal.fish-starship",
            "config.artifact.terminal.generator",
            "config.artifact.terminal.theme-service",
            "config.artifact.terminal.theme-path",
            "config.terminal.generated-state",
        }
        if include_hypr:
            required.add("config.hypr.takeover")
        seen = {item.id for item in actions}
        missing = sorted(required - seen)
        if missing:
            raise ConfigurationIntegrationError(
                "InstallationPlan lacks required Phase-11 actions: " + ", ".join(missing),
                code="RH_CONFIG_PLAN_CONTRACT_MISSING",
                details={"missing": missing},
            )

    def _verify_approved_preconditions(self, actions: tuple[ConfigAction, ...]) -> None:
        for action in actions:
            if not action.will_mutate or action.kind is ConfigActionKind.GENERATED_STATE:
                continue
            target = Path(action.target)
            current = fingerprint_path(target)
            if current != action.precondition_fingerprint:
                raise PreconditionFailedError(action.target, "target changed since InstallationPlan approval")
            self._assert_no_symlink_parent(target, self._boundary_for(target))

    def _apply_owned_file(self, action: ConfigAction) -> MutationResult:
        target = Path(action.target)
        self._ensure_directory(target.parent, allowed_root=self._boundary_for(target))
        content, mode = render_action_content(action, source_root=self.source_root)
        before = fingerprint_path(target)
        desired = fingerprint_regular_bytes(content, mode)
        if before == desired:
            return MutationResult(action.id, action.target, False, before, before, (), "already at desired release state")

        operation_id = new_operation_id()
        operation = WriteFileOperation(
            target=target,
            allowed_root=self._boundary_for(target),
            safety=OperationSafety(
                Reversibility.EXACT,
                capture_path_precondition(target),
                str(self.preimage_dir / f"{operation_id}.preimage"),
            ),
            operation_id=operation_id,
            content=content,
            mode=mode,
            preimage_path=self.preimage_dir / f"{operation_id}.preimage",
        )
        self.executor.execute(operation)
        self._operations.append(operation)
        return MutationResult(
            action.id,
            action.target,
            True,
            before,
            fingerprint_path(target),
            (operation.operation_id,),
            f"Realmheart-owned file installed mode={oct(mode)}",
        )

    def _apply_kitty_block(self, action: ConfigAction) -> MutationResult:
        target = Path(action.target)
        self._ensure_directory(target.parent, allowed_root=self._boundary_for(target))
        before = fingerprint_path(target)
        operation = plan_ensure_managed_block(
            target=target,
            allowed_root=self._boundary_for(target),
            preimage_dir=self.preimage_dir,
            block=ManagedBlock(KITTY_BEGIN, KITTY_END, kitty_managed_body(action)),
            default_mode=0o600,
        )
        if operation is None:
            return MutationResult(action.id, action.target, False, before, before, (), "exactly one canonical block already present")
        self.executor.execute(operation)
        self._operations.append(operation)
        return MutationResult(
            action.id,
            action.target,
            True,
            before,
            fingerprint_path(target),
            (operation.operation_id,),
            "shared Kitty file atomically replaced with one managed include block",
        )

    def _ensure_directory(self, directory: Path, *, allowed_root: Path) -> None:
        directory = Path(directory)
        allowed_root = Path(allowed_root)
        missing: list[Path] = []
        current = directory
        while not (current.exists() or current.is_symlink()):
            missing.append(current)
            if current == allowed_root:
                break
            current = current.parent
        if current.is_symlink():
            raise ConfigurationIntegrationError(
                f"Refusing configuration path through symlinked directory {current}",
                code="RH_CONFIG_SYMLINK_PARENT",
                details={"path": str(current)},
            )
        for path in reversed(missing):
            if path == allowed_root:
                # The caller's boundary is expected to exist. Never create or
                # mutate the safety root itself.
                raise ConfigurationIntegrationError(
                    f"Configuration safety root does not exist: {allowed_root}",
                    code="RH_CONFIG_ROOT_MISSING",
                )
            operation = CreateDirectoryOperation(
                target=path,
                allowed_root=allowed_root,
                safety=OperationSafety(Reversibility.EXACT, capture_path_precondition(path), None),
                mode=0o700,
            )
            self.executor.execute(operation)
            self._operations.append(operation)

    def _boundary_for(self, target: Path) -> Path:
        target_abs = Path(os.path.abspath(target))
        config_abs = Path(os.path.abspath(self.paths.config_home))
        try:
            if Path(os.path.commonpath([target_abs, config_abs])) == config_abs:
                return self.paths.config_home.parent
        except ValueError:
            pass
        raise ConfigurationIntegrationError(
            f"Phase-11 target lies outside XDG_CONFIG_HOME: {target}",
            code="RH_CONFIG_TARGET_OUTSIDE_SCOPE",
            details={"target": str(target), "config_home": str(self.paths.config_home)},
        )

    @staticmethod
    def _assert_no_symlink_parent(target: Path, boundary: Path) -> None:
        target = Path(os.path.abspath(target))
        boundary = Path(os.path.abspath(boundary))
        current = target.parent
        while current != boundary and current != current.parent:
            if current.is_symlink():
                raise ConfigurationIntegrationError(
                    f"Refusing configuration target beneath symlinked directory {current}",
                    code="RH_CONFIG_SYMLINK_PARENT",
                    details={"target": str(target), "symlink_parent": str(current)},
                )
            current = current.parent

    def _generated_paths(self) -> dict[str, Path]:
        result: dict[str, Path] = {}
        for artifact in self.plan.artifact_actions:
            if artifact.component_id == "terminal" and artifact.commit_class.value == "generated":
                result[artifact.artifact_id] = Path(artifact.target)
        return result

    def _capture_generated_preimages(self, paths: dict[str, Path]) -> None:
        self._generated_preimages = {}
        for artifact_id, path in paths.items():
            exists = path.exists() or path.is_symlink()
            if path.is_symlink():
                raise ConfigurationIntegrationError(
                    f"Generated terminal artifact target is a symlink: {path}",
                    code="RH_GENERATED_STATE_SYMLINK",
                    details={"artifact_id": artifact_id, "path": str(path)},
                )
            if exists and not path.is_file():
                raise ConfigurationIntegrationError(
                    f"Generated terminal artifact target is not a regular file: {path}",
                    code="RH_GENERATED_STATE_INVALID",
                    details={"artifact_id": artifact_id, "path": str(path)},
                )
            self._generated_preimages[artifact_id] = _GeneratedPreimage(
                path,
                exists,
                path.read_bytes() if exists else None,
                stat.S_IMODE(path.stat().st_mode) if exists else None,
                fingerprint_path(path),
            )

    def _run_generator(self, generated_paths: dict[str, Path]) -> None:
        python = self.runner.which("python3") or self.runner.which("python")
        if not python:
            raise ConfigurationIntegrationError("Python interpreter unavailable for terminal generator", code="RH_TERMINAL_GENERATOR_PYTHON_MISSING")
        generator = self.paths.config_home / "realmheart/scripts/terminal/generate-theme.py"
        palette = self.paths.state_home / "realmheart/theme-palette.tsv"
        palette_before = fingerprint_path(palette)
        opid = new_operation_id()
        self._generation_operation_id = opid
        self.journal.append(
            operation_id=opid,
            state=OperationState.INTENT,
            kind="generate_terminal_theme",
            target=str(self.paths.state_home / "realmheart/theme"),
            data={
                "generator": str(generator),
                "outputs": {key: str(value) for key, value in generated_paths.items()},
                "palette_input": str(palette),
                "palette_before": palette_before,
                "reversibility": Reversibility.BEST_EFFORT.value,
            },
        )
        self.journal.append(operation_id=opid, state=OperationState.STARTED, kind="generate_terminal_theme", target=str(generator))
        result = self.runner.run(
            [python, str(generator)],
            timeout=20.0,
            env={
                "HOME": str(self.paths.home),
                "XDG_CONFIG_HOME": str(self.paths.config_home),
                "XDG_STATE_HOME": str(self.paths.state_home),
            },
        )
        if not result.ok:
            self.journal.append(
                operation_id=opid,
                state=OperationState.FAILED,
                kind="generate_terminal_theme",
                target=str(generator),
                data={"returncode": result.returncode, "stderr": result.stderr[-1000:]},
            )
            raise ConfigurationIntegrationError(
                "Target-machine terminal theme generator failed",
                code="RH_TERMINAL_GENERATOR_FAILED",
                details={"returncode": result.returncode, "stderr": result.stderr[-1000:]},
            )
        missing = [key for key, path in generated_paths.items() if not path.is_file() or path.is_symlink() or path.stat().st_size == 0]
        if missing:
            self.journal.append(
                operation_id=opid,
                state=OperationState.FAILED,
                kind="generate_terminal_theme",
                target=str(generator),
                data={"missing_outputs": missing},
            )
            raise ConfigurationIntegrationError(
                "Terminal generator did not produce every canonical generated artifact",
                code="RH_TERMINAL_GENERATED_OUTPUT_MISSING",
                details={"missing": missing},
            )
        palette_after = fingerprint_path(palette)
        if palette_after != palette_before:
            self._warnings.append("theme-palette.tsv changed concurrently during terminal generation; generated output reflects whichever complete palette the generator accepted")
        self.journal.append(
            operation_id=opid,
            state=OperationState.COMPLETED,
            kind="generate_terminal_theme",
            target=str(generator),
            data={"after": {key: fingerprint_path(path) for key, path in generated_paths.items()}, "palette_after": palette_after},
        )

    def _capture_unit_state(self) -> UnitState:
        systemctl = self.runner.which("systemctl")
        if not systemctl:
            raise ConfigurationIntegrationError("systemctl is unavailable for terminal watcher integration", code="RH_TERMINAL_SYSTEMCTL_MISSING")
        enabled = self.runner.run([systemctl, "--user", "is-enabled", "realmheart-terminal-theme.path"], timeout=8.0)
        active = self.runner.run([systemctl, "--user", "is-active", "realmheart-terminal-theme.path"], timeout=8.0)
        enabled_text = (enabled.stdout or enabled.stderr or "disabled").strip().splitlines()[0]
        active_text = (active.stdout or active.stderr or "inactive").strip().splitlines()[0]
        return UnitState(enabled.returncode == 0 and enabled_text.startswith("enabled"), active.returncode == 0 and active_text == "active", enabled_text, active_text)

    def _activate_terminal_watcher(self) -> ServiceIntegrationResult:
        systemctl = self.runner.which("systemctl")
        if not systemctl:
            return ServiceIntegrationResult(True, None, None, (), False, "systemctl executable not found")
        before = self._capture_unit_state()
        self._service_before = before
        opid = new_operation_id()
        self._service_operation_id = opid
        commands = (
            (systemctl, "--user", "daemon-reload"),
            (systemctl, "--user", "enable", "--now", "realmheart-terminal-theme.path"),
            (systemctl, "--user", "restart", "realmheart-terminal-theme.service"),
        )
        self.journal.append(
            operation_id=opid,
            state=OperationState.INTENT,
            kind="systemd_user_watcher",
            target="realmheart-terminal-theme.path",
            data={"before": {"enabled": before.enabled, "active": before.active, "enabled_text": before.enabled_text, "active_text": before.active_text}},
        )
        self.journal.append(operation_id=opid, state=OperationState.STARTED, kind="systemd_user_watcher", target="realmheart-terminal-theme.path")
        self._service_mutated = True
        for command in commands:
            result = self.runner.run(command, timeout=20.0)
            if not result.ok:
                self.journal.append(
                    operation_id=opid,
                    state=OperationState.FAILED,
                    kind="systemd_user_watcher",
                    target="realmheart-terminal-theme.path",
                    data={"command": command, "returncode": result.returncode, "stderr": result.stderr[-1000:]},
                )
                return ServiceIntegrationResult(True, before, None, commands, False, f"watcher command failed: {' '.join(command)}", opid)
        after = self._capture_unit_state()
        ok = after.enabled and after.active
        self.journal.append(
            operation_id=opid,
            state=OperationState.COMPLETED if ok else OperationState.FAILED,
            kind="systemd_user_watcher",
            target="realmheart-terminal-theme.path",
            data={"after": {"enabled": after.enabled, "active": after.active, "enabled_text": after.enabled_text, "active_text": after.active_text}},
        )
        return ServiceIntegrationResult(True, before, after, commands, ok, "enabled+active" if ok else "path unit did not reach enabled+active", opid)

    def _quiesce_service_for_rollback(self) -> None:
        if not self._service_mutated or self._service_before is None:
            return
        systemctl = self.runner.which("systemctl")
        if not systemctl:
            self._warnings.append("systemctl unavailable during watcher rollback")
            return
        before = self._service_before
        if not before.active:
            self.runner.run([systemctl, "--user", "stop", "realmheart-terminal-theme.path"], timeout=15.0)
        if not before.enabled:
            self.runner.run([systemctl, "--user", "disable", "realmheart-terminal-theme.path"], timeout=15.0)

    def _restore_service_after_files(self) -> None:
        if not self._service_mutated or self._service_before is None:
            return
        systemctl = self.runner.which("systemctl")
        opid = self._service_operation_id
        if not systemctl or not opid:
            return
        self.journal.append(operation_id=opid, state=OperationState.ROLLBACK_STARTED, kind="systemd_user_watcher", target="realmheart-terminal-theme.path")
        ok = True
        for command in ((systemctl, "--user", "daemon-reload"),):
            ok = self.runner.run(command, timeout=15.0).ok and ok
        before = self._service_before
        if before.enabled:
            ok = self.runner.run([systemctl, "--user", "enable", "realmheart-terminal-theme.path"], timeout=15.0).ok and ok
        else:
            self.runner.run([systemctl, "--user", "disable", "realmheart-terminal-theme.path"], timeout=15.0)
        if before.active:
            ok = self.runner.run([systemctl, "--user", "start", "realmheart-terminal-theme.path"], timeout=15.0).ok and ok
        else:
            self.runner.run([systemctl, "--user", "stop", "realmheart-terminal-theme.path"], timeout=15.0)
        self.journal.append(
            operation_id=opid,
            state=OperationState.ROLLED_BACK if ok else OperationState.ROLLBACK_FAILED,
            kind="systemd_user_watcher",
            target="realmheart-terminal-theme.path",
        )
        if not ok:
            raise ConfigurationIntegrationError("Failed to restore previous terminal watcher state", code="RH_TERMINAL_WATCHER_ROLLBACK_FAILED")

    def _restore_generated_state(self) -> None:
        if not self._generated_preimages:
            return
        opid = self._generation_operation_id or new_operation_id()
        self.journal.append(operation_id=opid, state=OperationState.ROLLBACK_STARTED, kind="generate_terminal_theme", target=str(self.paths.state_home / "realmheart/theme"))
        ok = True
        for artifact_id, preimage in self._generated_preimages.items():
            current = fingerprint_path(preimage.path)
            expected_after = self._generated_after.get(artifact_id)
            if expected_after is not None and current != expected_after:
                self._warnings.append(f"generated artifact drifted after transaction; guarded rollback skipped {preimage.path}")
                ok = False
                continue
            try:
                if preimage.existed:
                    assert preimage.content is not None and preimage.mode is not None
                    _atomic_write(preimage.path, preimage.content, preimage.mode)
                elif preimage.path.exists() or preimage.path.is_symlink():
                    if preimage.path.is_dir() and not preimage.path.is_symlink():
                        ok = False
                    else:
                        preimage.path.unlink()
            except OSError:
                ok = False
        theme_dir = self.paths.state_home / "realmheart/theme"
        try:
            theme_dir.rmdir()
        except OSError:
            pass
        self.journal.append(
            operation_id=opid,
            state=OperationState.ROLLED_BACK if ok else OperationState.ROLLBACK_FAILED,
            kind="generate_terminal_theme",
            target=str(theme_dir),
        )
        if not ok:
            raise ConfigurationIntegrationError("Generated terminal state rollback was not exact", code="RH_GENERATED_STATE_ROLLBACK_FAILED")

    def _cleanup_unactivated_hypr_stage(self) -> None:
        stage = self._hypr_stage
        if stage is None:
            return
        if stage.staging.exists() and fingerprint_path(stage.staging) == stage.staging_fingerprint:
            shutil.rmtree(stage.staging)
        if stage.old.exists() and self._hypr_swap is None:
            # old is only created by a swap; seeing it without a swap object is
            # an invariant violation, so never guess-delete it.
            self._warnings.append(f"unexpected old Hypr staging path retained: {stage.old}")

    def _has_live_mutation(self) -> bool:
        return bool(self._operations or (self._hypr_swap and self._hypr_swap.operations) or self._generation_operation_id or self._service_mutated)


def _atomic_write(path: Path, content: bytes, mode: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(f".{path.name}.rh-restore-{uuid.uuid4().hex[:8]}")
    fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temp, mode)
        os.replace(temp, path)
        fsync_directory(path.parent)
    finally:
        if temp.exists() or temp.is_symlink():
            temp.unlink(missing_ok=True)
