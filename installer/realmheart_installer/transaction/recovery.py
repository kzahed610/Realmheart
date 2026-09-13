"""Crash-recovery inspection and conservative rollback for WAL operations."""

from __future__ import annotations

import json
import os
import shutil
from dataclasses import dataclass
from datetime import datetime, timezone
from enum import Enum
from pathlib import Path
from typing import Any, Iterable

from ..durability import fsync_directory
from ..errors import JournalCorruptError, OperationExecutionError, UnsafePathError
from ..filesystem.compare import fingerprint_path
from ..models import OperationKind, OperationState, TransactionState
from .journal import JournalEvent, WriteAheadJournal, read_journal

# Only states whose filesystem outcome is unambiguous are clean at the
# operation level. FAILED may follow a partially-applied mutation, and
# ROLLBACK_FAILED explicitly requires manual/recovery attention.
_CLEAN_OPERATION_STATES = {
    OperationState.COMPLETED,
    OperationState.ROLLED_BACK,
}


class RecoveryDisposition(str, Enum):
    NOT_APPLIED = "not_applied"
    APPLIED = "applied"
    AMBIGUOUS = "ambiguous"
    ALREADY_ROLLED_BACK = "already_rolled_back"
    COMPLETE = "complete"


class RecoveryStatus(str, Enum):
    CLEAN = "clean"
    RECOVERY_AVAILABLE = "recovery_available"
    MANUAL_ATTENTION = "manual_attention"


@dataclass(frozen=True)
class RecoveryOperationReport:
    operation_id: str
    kind: str | None
    target: str | None
    latest_state: str
    disposition: str
    automatically_reversible: bool


@dataclass(frozen=True)
class RecoveryReport:
    schema_version: int
    transaction_id: str
    generated_at: str
    trigger: str
    transaction_state: str
    status: RecoveryStatus
    journal_available: bool
    journal_error: str | None
    operations: tuple[RecoveryOperationReport, ...]
    incomplete_operation_count: int
    automatic_rollback_safe: bool
    primary_error: str | None
    rollback_errors: tuple[str, ...]
    note: str


@dataclass(frozen=True)
class RecoveryCandidate:
    transaction_id: str
    transaction_dir: str
    transaction_state: str
    status: RecoveryStatus
    automatic_rollback_safe: bool
    blocks_new_transaction: bool
    recovery_report: str
    load_error: str | None = None


@dataclass(frozen=True)
class RecoveredOperation:
    operation_id: str
    kind: str | None
    target: str | None
    latest_state: OperationState
    intent: JournalEvent | None
    events: tuple[JournalEvent, ...]

    @property
    def requires_recovery_inspection(self) -> bool:
        return self.latest_state not in _CLEAN_OPERATION_STATES


@dataclass(frozen=True)
class RecoveryInspection:
    operations: tuple[RecoveredOperation, ...]

    @property
    def incomplete_operations(self) -> tuple[RecoveredOperation, ...]:
        return tuple(op for op in self.operations if op.requires_recovery_inspection)

    @property
    def is_clean(self) -> bool:
        return not self.incomplete_operations


def inspect_journal(path: Path) -> RecoveryInspection:
    events = read_journal(path)
    order: list[str] = []
    grouped: dict[str, list[JournalEvent]] = {}
    for event in events:
        if event.operation_id not in grouped:
            grouped[event.operation_id] = []
            order.append(event.operation_id)
        grouped[event.operation_id].append(event)

    operations: list[RecoveredOperation] = []
    for operation_id in order:
        op_events = grouped[operation_id]
        intent = next((event for event in op_events if event.state == OperationState.INTENT), None)
        latest = op_events[-1]
        operations.append(
            RecoveredOperation(
                operation_id=operation_id,
                kind=intent.kind if intent else latest.kind,
                target=intent.target if intent else latest.target,
                latest_state=latest.state,
                intent=intent,
                events=tuple(op_events),
            )
        )
    return RecoveryInspection(tuple(operations))


def classify_recovered_operation(operation: RecoveredOperation) -> RecoveryDisposition:
    """Classify operation-level recovery status."""

    if operation.latest_state == OperationState.COMPLETED:
        return RecoveryDisposition.COMPLETE
    if operation.latest_state == OperationState.ROLLED_BACK:
        return RecoveryDisposition.ALREADY_ROLLED_BACK
    return _classify_filesystem_effect(operation)


def _classify_filesystem_effect(operation: RecoveredOperation) -> RecoveryDisposition:
    """Inspect current paths against durable before/after intent metadata."""

    if operation.intent is None or operation.target is None or operation.kind is None:
        return RecoveryDisposition.AMBIGUOUS

    data = operation.intent.data or {}
    target = Path(operation.target)
    try:
        kind = OperationKind(operation.kind)
    except ValueError:
        # Privileged/service operations have their own higher-level rollback
        # handlers.  Generic crash recovery must never guess their semantics.
        return RecoveryDisposition.AMBIGUOUS

    if kind == OperationKind.WRITE_FILE:
        before = data.get("before_fingerprint")
        after = data.get("expected_after_fingerprint")
        current = fingerprint_path(target)
        if before and current == before:
            return RecoveryDisposition.NOT_APPLIED
        if after and current == after:
            if bool(data.get("existed_before")):
                preimage_raw = data.get("preimage_path")
                if not preimage_raw or not Path(str(preimage_raw)).exists():
                    return RecoveryDisposition.AMBIGUOUS
            return RecoveryDisposition.APPLIED
        return RecoveryDisposition.AMBIGUOUS

    if kind == OperationKind.CREATE_DIRECTORY:
        if not (target.exists() or target.is_symlink()):
            return RecoveryDisposition.NOT_APPLIED
        if target.is_dir() and not target.is_symlink():
            try:
                next(target.iterdir())
            except StopIteration:
                return RecoveryDisposition.APPLIED
        return RecoveryDisposition.AMBIGUOUS

    if kind == OperationKind.MOVE_PATH:
        destination_raw = data.get("destination")
        before = data.get("before_fingerprint")
        if not destination_raw or not before:
            return RecoveryDisposition.AMBIGUOUS
        destination = Path(destination_raw)
        source_exists = target.exists() or target.is_symlink()
        dest_exists = destination.exists() or destination.is_symlink()
        if source_exists and not dest_exists and fingerprint_path(target) == before:
            return RecoveryDisposition.NOT_APPLIED
        if not source_exists and dest_exists and fingerprint_path(destination) == before:
            return RecoveryDisposition.APPLIED
        return RecoveryDisposition.AMBIGUOUS

    if kind == OperationKind.REMOVE_PATH:
        backup_raw = data.get("backup_path")
        before = data.get("before_fingerprint")
        if not backup_raw or not before:
            return RecoveryDisposition.AMBIGUOUS
        backup = Path(backup_raw)
        target_exists = target.exists() or target.is_symlink()
        backup_exists = backup.exists() or backup.is_symlink()
        if target_exists and not backup_exists and fingerprint_path(target) == before:
            return RecoveryDisposition.NOT_APPLIED
        if not target_exists and backup_exists and fingerprint_path(backup) == before:
            return RecoveryDisposition.APPLIED
        return RecoveryDisposition.AMBIGUOUS

    return RecoveryDisposition.AMBIGUOUS


def rollback_recovered_operation(
    operation: RecoveredOperation,
    *,
    approved_roots: Iterable[Path],
    journal: WriteAheadJournal,
    rollback_completed: bool = False,
) -> RecoveryDisposition:
    """Rollback one ambiguous/interrupted operation using only durable WAL state.

    The caller supplies trusted roots from the freshly-resolved current installer
    context. Journal paths never authorize themselves.
    """

    if operation.latest_state == OperationState.ROLLED_BACK:
        return RecoveryDisposition.ALREADY_ROLLED_BACK
    if operation.latest_state == OperationState.COMPLETED and not rollback_completed:
        return RecoveryDisposition.COMPLETE

    # For transaction-level rollback, inspect the current filesystem even for
    # operations whose COMPLETED record was durable. Later operations may still
    # have failed, requiring earlier completed mutations to be reversed.
    disposition = _classify_filesystem_effect(operation)
    if disposition == RecoveryDisposition.NOT_APPLIED:
        # A durable INTENT/STARTED record whose mutation provably never reached
        # the filesystem is still a resolved recovery outcome.  Record the
        # no-op rollback so this transaction does not remain an eternal
        # "incomplete operation" on every future startup.
        journal.append(
            operation_id=operation.operation_id,
            state=OperationState.ROLLED_BACK,
            kind=operation.kind,
            target=operation.target,
            data={"recovery": True, "noop": True},
        )
        return disposition
    if disposition != RecoveryDisposition.APPLIED:
        raise OperationExecutionError(operation.operation_id, "recovery state is ambiguous; refusing automatic rollback")
    if operation.intent is None or operation.target is None or operation.kind is None:
        raise OperationExecutionError(operation.operation_id, "recovery intent metadata is incomplete")

    data = operation.intent.data or {}
    target = Path(operation.target)
    _require_approved(target, approved_roots)

    journal.append(
        operation_id=operation.operation_id,
        state=OperationState.ROLLBACK_STARTED,
        kind=operation.kind,
        target=operation.target,
        data={"recovered_from": operation.latest_state.value},
    )
    try:
        kind = OperationKind(operation.kind)
        if kind == OperationKind.CREATE_DIRECTORY:
            target.rmdir()
            fsync_directory(target.parent)

        elif kind == OperationKind.WRITE_FILE:
            expected_after = data.get("expected_after_fingerprint")
            if fingerprint_path(target) != expected_after:
                raise OperationExecutionError(operation.operation_id, "write target changed after crash")
            existed_before = bool(data.get("existed_before"))
            if not existed_before:
                target.unlink()
                fsync_directory(target.parent)
            else:
                preimage_raw = data.get("preimage_path")
                if not preimage_raw:
                    raise OperationExecutionError(operation.operation_id, "missing durable write preimage")
                preimage = Path(preimage_raw)
                _require_approved(preimage, approved_roots, allow_root_itself=True)
                if not preimage.exists():
                    raise OperationExecutionError(operation.operation_id, "write preimage is missing")
                target.unlink()
                if data.get("was_symlink"):
                    link_target = data.get("symlink_target")
                    if not isinstance(link_target, str):
                        metadata = json.loads(preimage.read_text(encoding="utf-8"))
                        link_target = metadata["target"]
                    os.symlink(link_target, target)
                    fsync_directory(target.parent)
                else:
                    shutil.copy2(preimage, target, follow_symlinks=False)
                    with target.open("rb") as handle:
                        os.fsync(handle.fileno())
                    fsync_directory(target.parent)

        elif kind == OperationKind.MOVE_PATH:
            destination = Path(str(data["destination"]))
            _require_approved(destination, approved_roots)
            if target.exists() or target.is_symlink():
                raise OperationExecutionError(operation.operation_id, "move source path is occupied during recovery")
            os.replace(destination, target)
            fsync_directory(target.parent)
            if destination.parent != target.parent:
                fsync_directory(destination.parent)

        elif kind == OperationKind.REMOVE_PATH:
            backup = Path(str(data["backup_path"]))
            _require_approved(backup, approved_roots)
            if target.exists() or target.is_symlink():
                raise OperationExecutionError(operation.operation_id, "remove target is occupied during recovery")
            os.replace(backup, target)
            fsync_directory(target.parent)
            if backup.parent != target.parent:
                fsync_directory(backup.parent)
        else:
            raise OperationExecutionError(operation.operation_id, f"unsupported recovery kind {kind.value}")
    except Exception as exc:
        journal.append(
            operation_id=operation.operation_id,
            state=OperationState.ROLLBACK_FAILED,
            kind=operation.kind,
            target=operation.target,
            data={"error_type": type(exc).__name__, "error": str(exc)},
        )
        raise

    journal.append(
        operation_id=operation.operation_id,
        state=OperationState.ROLLED_BACK,
        kind=operation.kind,
        target=operation.target,
        data={"recovery": True},
    )
    return RecoveryDisposition.APPLIED


def rollback_transaction_from_journal(
    journal_path: Path,
    *,
    approved_roots: Iterable[Path],
) -> None:
    """Reverse all still-applied journal operations in reverse operation order."""

    inspection = inspect_journal(journal_path)
    journal = WriteAheadJournal(journal_path)
    for operation in reversed(inspection.operations):
        if operation.latest_state == OperationState.ROLLED_BACK:
            continue
        rollback_recovered_operation(
            operation,
            approved_roots=approved_roots,
            journal=journal,
            rollback_completed=True,
        )


def build_recovery_report(
    context: Any,
    *,
    trigger: str,
    primary_error: BaseException | str | None = None,
    rollback_errors: Iterable[str] = (),
) -> RecoveryReport:
    """Build a transaction-local recovery report without mutating live state.

    This report is intentionally simpler than the shareable Phase-15 diagnostic
    bundle: its job is to explain transaction/recovery state even when normal
    verification never ran or the machine is under resource pressure.
    """

    journal_error: str | None = None
    journal_available = context.journal_path.is_file()
    operations: tuple[RecoveredOperation, ...] = ()
    if journal_available:
        try:
            operations = inspect_journal(context.journal_path).operations
        except (JournalCorruptError, OSError, ValueError) as exc:
            journal_error = f"{type(exc).__name__}: {exc}"

    state = context.transaction.state
    transaction_needs_rollback = state not in {TransactionState.COMMITTED, TransactionState.ROLLED_BACK}
    rendered: list[RecoveryOperationReport] = []
    incomplete = 0
    all_reversible = journal_error is None
    generic_kinds = {item.value for item in OperationKind}
    for operation in operations:
        if operation.requires_recovery_inspection:
            incomplete += 1
        generic = operation.kind in generic_kinds
        trusted_paths = generic and _operation_paths_are_approved(operation, approved_recovery_roots(context))
        try:
            if not trusted_paths:
                disposition = RecoveryDisposition.AMBIGUOUS
            elif transaction_needs_rollback and operation.latest_state is OperationState.COMPLETED:
                disposition = _classify_filesystem_effect(operation)
            else:
                disposition = classify_recovered_operation(operation)
        except Exception:
            disposition = RecoveryDisposition.AMBIGUOUS
        reversible = generic and operation.latest_state is not OperationState.ROLLBACK_FAILED and disposition in {
            RecoveryDisposition.NOT_APPLIED,
            RecoveryDisposition.APPLIED,
            RecoveryDisposition.ALREADY_ROLLED_BACK,
            RecoveryDisposition.COMPLETE,
        }
        # A completed non-generic operation may be perfectly healthy, but if the
        # enclosing transaction needs rollback, the generic recovery engine has
        # no authority to reverse it.
        if not reversible and operation.latest_state != OperationState.ROLLED_BACK:
            all_reversible = False
        rendered.append(RecoveryOperationReport(
            operation_id=operation.operation_id,
            kind=operation.kind,
            target=operation.target,
            latest_state=operation.latest_state.value,
            disposition=disposition.value,
            automatically_reversible=reversible,
        ))

    rollback_errors_tuple = tuple(str(item) for item in rollback_errors)
    terminal_state = state in {TransactionState.COMMITTED, TransactionState.ROLLED_BACK}
    terminal_clean = terminal_state and journal_error is None and incomplete == 0
    if terminal_state and not terminal_clean:
        # A terminal transaction summary cannot overrule contradictory durable
        # WAL evidence.  Treat the mismatch as a forensic inconsistency rather
        # than silently blessing an interrupted/corrupt operation.
        all_reversible = False
    manual_acknowledged = bool(context.transaction.metadata.get("manual_recovery_acknowledged"))
    live_mutation_started = bool(context.transaction.metadata.get("live_mutation_started"))
    external_package_action = (
        bool(context.transaction.metadata.get("package_install_started"))
        or bool(context.transaction.metadata.get("package_cleanup_started"))
        or context.transaction.metadata.get("package_install") is not None
        or context.transaction.metadata.get("package_cleanup") is not None
    )
    if external_package_action and not terminal_clean:
        # Package-manager operations are BEST_EFFORT and are intentionally not
        # reconstructed by the generic filesystem WAL.
        all_reversible = False
    no_recovery_needed = not journal_available and not live_mutation_started and not external_package_action
    if manual_acknowledged:
        status = RecoveryStatus.CLEAN
    elif (terminal_clean or no_recovery_needed) and not rollback_errors_tuple and journal_error is None:
        status = RecoveryStatus.CLEAN
    elif all_reversible and not rollback_errors_tuple:
        status = RecoveryStatus.RECOVERY_AVAILABLE
    else:
        status = RecoveryStatus.MANUAL_ATTENTION

    error_text = None
    if primary_error is not None:
        error_text = str(primary_error)
        if not isinstance(primary_error, str):
            error_text = f"{type(primary_error).__name__}: {primary_error}"

    note = (
        "manual recovery was explicitly acknowledged; no automatic repair is being claimed" if manual_acknowledged else
        "no transaction-owned recovery action is required" if status is RecoveryStatus.CLEAN else
        "generic WAL rollback is mechanically available; re-entry must still use trusted current roots" if status is RecoveryStatus.RECOVERY_AVAILABLE else
        "automatic recovery is not proven safe; preserve transaction artifacts and inspect before further mutation"
    )
    return RecoveryReport(
        schema_version=1,
        transaction_id=context.transaction.transaction_id,
        generated_at=datetime.now(timezone.utc).isoformat(),
        trigger=trigger,
        transaction_state=state.value,
        status=status,
        journal_available=journal_available,
        journal_error=journal_error,
        operations=tuple(rendered),
        incomplete_operation_count=incomplete,
        automatic_rollback_safe=(status is RecoveryStatus.RECOVERY_AVAILABLE),
        primary_error=error_text,
        rollback_errors=rollback_errors_tuple,
        note=note,
    )


def persist_recovery_report(
    context: Any,
    *,
    trigger: str,
    primary_error: BaseException | str | None = None,
    rollback_errors: Iterable[str] = (),
) -> RecoveryReport:
    """Persist ``recovery.json`` without ever masking the original failure.

    ``InstallContext.persist_recovery_json`` releases the preallocated recovery
    reserve and retries once on ENOSPC.  If even that cannot persist, callers
    still receive the in-memory report and the primary installer error remains
    authoritative.
    """

    report = build_recovery_report(
        context, trigger=trigger, primary_error=primary_error, rollback_errors=rollback_errors,
    )
    try:
        context.persist_recovery_json("recovery.json", report)
        # Keep emergency space reserved while recovery is still unresolved; a
        # later rollback/report update may itself run under disk pressure.  The
        # ENOSPC retry path sacrifices the reserve automatically when needed.
        if report.status is RecoveryStatus.CLEAN:
            context.release_recovery_reserve()
    except Exception as exc:
        # There is no safe write left to make here.  Do not recursively attempt
        # another summary write; that could mask the actual installer failure.
        try:
            context.transaction.metadata["recovery_report_persistence_error"] = f"{type(exc).__name__}: {exc}"
        except Exception:
            pass
    return report


def discover_recovery_candidates(paths: Any, *, persist_reports: bool = True) -> tuple[RecoveryCandidate, ...]:
    """Find abandoned transactions that still need recovery attention.

    A clean preflight/planning crash does not block a future install merely
    because its transaction summary is nonterminal.  We reconstruct the durable
    WAL first and only return transactions whose report says recovery work or
    manual inspection is actually required.
    """

    transactions = Path(paths.transactions)
    if not transactions.is_dir():
        return ()

    from ..context import InstallContext

    candidates: list[RecoveryCandidate] = []
    for transaction_dir in sorted((item for item in transactions.iterdir() if item.is_dir() and not item.is_symlink()), key=lambda item: item.name):
        if not transaction_dir.name.startswith("RH-"):
            continue
        recovery_path = transaction_dir / "recovery.json"
        try:
            context = InstallContext.load_existing(paths=paths, transaction_id=transaction_dir.name)
            report = (
                persist_recovery_report(context, trigger="startup_reentry")
                if persist_reports else
                build_recovery_report(context, trigger="startup_reentry")
            )
            if report.status is RecoveryStatus.CLEAN:
                continue
            blocking_states = {
                TransactionState.APPLYING, TransactionState.VERIFYING, TransactionState.DECISION_REQUIRED,
                TransactionState.COMMITTING, TransactionState.INTERRUPTED, TransactionState.ROLLING_BACK,
                TransactionState.ROLLBACK_FAILED,
            }
            blocks_new = (
                context.transaction.state in blocking_states
                or (context.transaction.state is TransactionState.FAILED and bool(context.transaction.metadata.get("live_mutation_started")))
            )
            candidates.append(RecoveryCandidate(
                transaction_id=context.transaction.transaction_id,
                transaction_dir=str(transaction_dir),
                transaction_state=context.transaction.state.value,
                status=report.status,
                automatic_rollback_safe=report.automatic_rollback_safe,
                blocks_new_transaction=blocks_new,
                recovery_report=str(recovery_path),
            ))
        except Exception as exc:
            error = f"{type(exc).__name__}: {exc}"
            if persist_reports:
                _persist_unloadable_recovery_report(
                    transaction_dir, transaction_id=transaction_dir.name, error=error,
                )
            candidates.append(RecoveryCandidate(
                transaction_id=transaction_dir.name,
                transaction_dir=str(transaction_dir),
                transaction_state="unknown",
                status=RecoveryStatus.MANUAL_ATTENTION,
                automatic_rollback_safe=False,
                blocks_new_transaction=True,
                recovery_report=str(recovery_path),
                load_error=error,
            ))
    return tuple(candidates)


def _operation_paths_are_approved(operation: RecoveredOperation, approved_roots: Iterable[Path]) -> bool:
    """Authorize durable WAL paths lexically before any filesystem inspection."""

    if operation.intent is None or operation.target is None or operation.kind is None:
        return False
    try:
        kind = OperationKind(operation.kind)
    except ValueError:
        return False
    roots = tuple(approved_roots)
    try:
        _require_approved(Path(operation.target), roots)
        data = operation.intent.data or {}
        journal_root = data.get("allowed_root")
        if journal_root:
            _require_approved(Path(str(journal_root)), roots, allow_root_itself=True)
            target_abs = Path(os.path.abspath(os.path.expanduser(operation.target)))
            allowed_abs = Path(os.path.abspath(os.path.expanduser(str(journal_root))))
            if Path(os.path.commonpath([target_abs, allowed_abs])) != allowed_abs or target_abs == allowed_abs:
                return False
        if kind is OperationKind.WRITE_FILE and data.get("preimage_path"):
            _require_approved(Path(str(data["preimage_path"])), roots)
        elif kind is OperationKind.MOVE_PATH and data.get("destination"):
            _require_approved(Path(str(data["destination"])), roots)
        elif kind is OperationKind.REMOVE_PATH and data.get("backup_path"):
            _require_approved(Path(str(data["backup_path"])), roots)
    except (UnsafePathError, ValueError):
        return False
    return True


def approved_recovery_roots(context: Any) -> tuple[Path, ...]:
    """Return trusted roots for generic WAL recovery.

    System paths are intentionally absent.  Privileged/system operations use
    custom journal kinds and therefore require their higher-level/manual recovery
    path; a historical journal can never authorize arbitrary root filesystem
    writes merely by naming a path.
    """

    paths = context.paths
    roots = (
        paths.config_home, paths.state_home, paths.data_home, paths.cache_home,
        paths.home / ".local/bin",
        paths.installer_state, paths.installer_data, paths.installer_cache, context.transaction_dir,
    )
    unique: list[Path] = []
    seen: set[str] = set()
    for root in roots:
        normalized = str(Path(root).absolute())
        if normalized not in seen:
            seen.add(normalized)
            unique.append(Path(root))
    return tuple(unique)


def recover_transaction_from_journal(context: Any) -> RecoveryReport:
    """Perform a clean-reentry rollback when the report proves it is safe."""

    before = build_recovery_report(context, trigger="explicit_recovery_inspect")
    if before.status is not RecoveryStatus.RECOVERY_AVAILABLE or not before.automatic_rollback_safe:
        raise OperationExecutionError(
            context.transaction.transaction_id,
            "automatic recovery is not proven safe; inspect recovery.json and resolve manually",
        )

    context.transaction.transition(TransactionState.ROLLING_BACK)
    context.persist_summary()
    try:
        rollback_transaction_from_journal(
            context.journal_path, approved_roots=approved_recovery_roots(context),
        )
    except Exception as exc:
        context.transaction.transition(TransactionState.ROLLBACK_FAILED)
        context.transaction.metadata["reentry_rollback_error"] = f"{type(exc).__name__}: {exc}"
        try:
            context.persist_summary()
        finally:
            return persist_recovery_report(
                context, trigger="explicit_recovery_rollback_failed",
                primary_error=exc, rollback_errors=(str(exc),),
            )

    context.transaction.transition(TransactionState.ROLLED_BACK)
    context.transaction.metadata["recovered_on_reentry"] = True
    context.persist_summary()
    return persist_recovery_report(context, trigger="explicit_recovery_rollback")


def acknowledge_manual_recovery(context: Any) -> RecoveryReport:
    """Close a manual-attention incident after the user repaired/accepted it.

    This deliberately does not claim that Realmheart performed a rollback.  It
    records an explicit operator acknowledgement so future transactions are not
    permanently deadlocked by an incident that cannot be mechanically reversed.
    """

    before = build_recovery_report(context, trigger="manual_recovery_acknowledge_check")
    if before.status is not RecoveryStatus.MANUAL_ATTENTION:
        raise OperationExecutionError(
            context.transaction.transaction_id,
            "manual acknowledgement is only valid for a manual-attention recovery incident",
        )
    context.transaction.metadata["manual_recovery_acknowledged"] = True
    context.transaction.metadata["manual_recovery_previous_state"] = context.transaction.state.value
    context.transaction.metadata["manual_recovery_acknowledged_at"] = datetime.now(timezone.utc).isoformat()
    context.transaction.transition(TransactionState.FAILED)
    context.persist_summary()
    return persist_recovery_report(context, trigger="manual_recovery_acknowledged")


def _persist_unloadable_recovery_report(transaction_dir: Path, *, transaction_id: str, error: str) -> None:
    """Best-effort report for a transaction whose summary itself is unreadable."""

    reserve = transaction_dir / ".recovery-reserve"
    target = transaction_dir / "recovery.json"
    payload = {
        "schema_version": 1,
        "transaction_id": transaction_id,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "trigger": "startup_reentry",
        "transaction_state": "unknown",
        "status": RecoveryStatus.MANUAL_ATTENTION.value,
        "journal_available": (transaction_dir / "journal.jsonl").is_file(),
        "journal_error": error,
        "operations": [],
        "incomplete_operation_count": 0,
        "automatic_rollback_safe": False,
        "primary_error": error,
        "rollback_errors": [],
        "note": "transaction summary could not be reconstructed; preserve this directory and inspect manually",
    }
    encoded = json.dumps(payload, indent=2, sort_keys=True) + "\n"

    def write_once() -> None:
        temporary = target.with_name(f".{target.name}.tmp")
        with temporary.open("w", encoding="utf-8") as handle:
            handle.write(encoded)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, target)
        fsync_directory(target.parent)

    try:
        write_once()
    except OSError as exc:
        import errno
        if exc.errno != errno.ENOSPC:
            return
        try:
            reserve.unlink(missing_ok=True)
            write_once()
        except OSError:
            return


def _require_approved(path: Path, approved_roots: Iterable[Path], *, allow_root_itself: bool = False) -> None:
    path_abs = Path(os.path.abspath(os.path.expanduser(str(path))))
    for root in approved_roots:
        root_abs = Path(os.path.abspath(os.path.expanduser(str(root))))
        try:
            common = Path(os.path.commonpath([path_abs, root_abs]))
        except ValueError:
            continue
        if common == root_abs and (allow_root_itself or path_abs != root_abs):
            return
    roots = ", ".join(str(root) for root in approved_roots)
    raise UnsafePathError(str(path_abs), roots, "recovery path is outside trusted roots")
