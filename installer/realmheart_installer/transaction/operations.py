"""Typed mutation primitives and write-ahead execution.

These primitives are the generic mutation kernel used by Realmheart live install,
uninstall, rollback, and crash recovery.
"""

from __future__ import annotations

import json
import os
import shutil
import stat
import uuid
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ..errors import OperationExecutionError, UnsafePathError
from ..durability import fsync_directory
from ..filesystem.compare import fingerprint_path, fingerprint_regular_bytes
from ..models import OperationKind, OperationSafety, OperationState, Reversibility, to_jsonable
from .journal import WriteAheadJournal
from .preconditions import verify_precondition


def new_operation_id() -> str:
    return f"op-{uuid.uuid4().hex[:12]}"


def _lexical_absolute(path: Path) -> Path:
    return Path(os.path.abspath(os.path.expanduser(str(path))))


def ensure_within_root(target: Path, allowed_root: Path, *, allow_root_itself: bool = False) -> None:
    target_abs = _lexical_absolute(target)
    root_abs = _lexical_absolute(allowed_root)
    try:
        common = Path(os.path.commonpath([target_abs, root_abs]))
    except ValueError as exc:
        raise UnsafePathError(str(target_abs), str(root_abs), "different filesystem namespace") from exc

    if common != root_abs:
        raise UnsafePathError(str(target_abs), str(root_abs), "target escapes allowed root")
    if target_abs == root_abs and not allow_root_itself:
        raise UnsafePathError(str(target_abs), str(root_abs), "refusing operation on allowed root itself")


@dataclass
class TransactionOperation(ABC):
    target: Path
    allowed_root: Path
    safety: OperationSafety
    operation_id: str = field(default_factory=new_operation_id)
    prepared: bool = False

    @property
    @abstractmethod
    def kind(self) -> OperationKind:
        raise NotImplementedError

    def prepare(self) -> None:
        ensure_within_root(self.target, self.allowed_root)
        self.prepared = True

    def verify_before_mutation(self) -> None:
        ensure_within_root(self.target, self.allowed_root)
        verify_precondition(self.target, self.safety.precondition)

    def journal_data(self) -> dict[str, Any]:
        return {
            "reversibility": self.safety.reversibility.value,
            "precondition": to_jsonable(self.safety.precondition),
            "rollback_source": self.safety.rollback_source,
            "allowed_root": str(self.allowed_root),
        }

    @abstractmethod
    def apply(self) -> None:
        raise NotImplementedError

    @abstractmethod
    def rollback(self) -> None:
        raise NotImplementedError


@dataclass
class WriteFileOperation(TransactionOperation):
    content: bytes = b""
    mode: int = 0o600
    preimage_path: Path | None = None
    _existed_before: bool = False
    _was_symlink: bool = False
    _symlink_target: str | None = None

    @property
    def kind(self) -> OperationKind:
        return OperationKind.WRITE_FILE

    def prepare(self) -> None:
        super().prepare()
        exists = self.target.exists() or self.target.is_symlink()
        self._existed_before = exists
        if not exists:
            return

        st = self.target.lstat()
        if stat.S_ISDIR(st.st_mode):
            raise OperationExecutionError(self.operation_id, "write_file target is a directory")

        if self.preimage_path is None:
            raise OperationExecutionError(self.operation_id, "exact write rollback requires a preimage path")
        self.preimage_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        if self.preimage_path.exists() or self.preimage_path.is_symlink():
            raise OperationExecutionError(self.operation_id, "write preimage path already exists")

        if stat.S_ISLNK(st.st_mode):
            self._was_symlink = True
            self._symlink_target = os.readlink(self.target)
            metadata = {
                "type": "symlink",
                "target": self._symlink_target,
            }
            _durable_write_bytes(self.preimage_path, json.dumps(metadata, sort_keys=True).encode("utf-8"))
        else:
            _durable_copy_file(self.target, self.preimage_path)

    def journal_data(self) -> dict[str, Any]:
        data = super().journal_data()
        data.update(
            {
                "mode": oct(self.mode),
                "existed_before": self._existed_before,
                "preimage_path": str(self.preimage_path) if self.preimage_path else None,
                "was_symlink": self._was_symlink,
                "symlink_target": self._symlink_target,
                "before_fingerprint": fingerprint_path(self.target),
                "expected_after_fingerprint": fingerprint_regular_bytes(self.content, self.mode),
            }
        )
        return data

    def apply(self) -> None:
        if not self.target.parent.is_dir():
            raise OperationExecutionError(self.operation_id, "write target parent does not exist")
        if self.target.is_symlink():
            self.target.unlink()
        temporary = self.target.with_name(f".{self.target.name}.rh-tmp-{self.operation_id}")
        try:
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
            fd = os.open(temporary, flags, self.mode)
            try:
                with os.fdopen(fd, "wb", closefd=False) as handle:
                    handle.write(self.content)
                    handle.flush()
                    os.fsync(handle.fileno())
                os.close(fd)
            except Exception:
                try:
                    os.close(fd)
                except OSError:
                    pass
                raise
            os.chmod(temporary, self.mode)
            os.replace(temporary, self.target)
            fsync_directory(self.target.parent)
        finally:
            if temporary.exists() or temporary.is_symlink():
                temporary.unlink(missing_ok=True)

    def rollback(self) -> None:
        if not self._existed_before:
            if self.target.exists() or self.target.is_symlink():
                self.target.unlink()
                fsync_directory(self.target.parent)
            return

        if self.preimage_path is None:
            raise OperationExecutionError(self.operation_id, "missing write-file preimage")

        if self.target.exists() or self.target.is_symlink():
            if self.target.is_dir() and not self.target.is_symlink():
                raise OperationExecutionError(self.operation_id, "rollback target unexpectedly became a directory")
            self.target.unlink()

        if self._was_symlink:
            if self._symlink_target is None:
                metadata = json.loads(self.preimage_path.read_text(encoding="utf-8"))
                self._symlink_target = metadata["target"]
            os.symlink(self._symlink_target, self.target)
        else:
            _durable_copy_file(self.preimage_path, self.target)
        fsync_directory(self.target.parent)


@dataclass
class CreateDirectoryOperation(TransactionOperation):
    mode: int = 0o700
    _created: bool = False

    @property
    def kind(self) -> OperationKind:
        return OperationKind.CREATE_DIRECTORY

    def apply(self) -> None:
        if self.target.exists():
            if not self.target.is_dir() or self.target.is_symlink():
                raise OperationExecutionError(self.operation_id, "target already exists and is not a directory")
            return
        self.target.mkdir(parents=False, mode=self.mode)
        self._created = True
        fsync_directory(self.target.parent)

    def rollback(self) -> None:
        if not self._created:
            return
        try:
            self.target.rmdir()
            fsync_directory(self.target.parent)
        except OSError as exc:
            raise OperationExecutionError(
                self.operation_id, "created directory is no longer empty; refusing destructive rollback"
            ) from exc


@dataclass
class MovePathOperation(TransactionOperation):
    destination: Path = Path(".")

    @property
    def kind(self) -> OperationKind:
        return OperationKind.MOVE_PATH

    def prepare(self) -> None:
        super().prepare()
        ensure_within_root(self.destination, self.allowed_root)
        if not (self.target.exists() or self.target.is_symlink()):
            raise OperationExecutionError(self.operation_id, "move source does not exist")
        if self.destination.exists() or self.destination.is_symlink():
            raise OperationExecutionError(self.operation_id, "move destination already exists")
        if not self.destination.parent.is_dir():
            raise OperationExecutionError(self.operation_id, "move destination parent does not exist")
        if self.target.parent.stat().st_dev != self.destination.parent.stat().st_dev:
            raise OperationExecutionError(self.operation_id, "EXACT move requires source and destination on same filesystem")

    def journal_data(self) -> dict[str, Any]:
        data = super().journal_data()
        data["destination"] = str(self.destination)
        data["before_fingerprint"] = fingerprint_path(self.target)
        return data

    def apply(self) -> None:
        os.replace(self.target, self.destination)
        fsync_directory(self.target.parent)
        if self.destination.parent != self.target.parent:
            fsync_directory(self.destination.parent)

    def rollback(self) -> None:
        if self.target.exists() or self.target.is_symlink():
            raise OperationExecutionError(self.operation_id, "move rollback source path is occupied")
        if not (self.destination.exists() or self.destination.is_symlink()):
            raise OperationExecutionError(self.operation_id, "move rollback destination is missing")
        os.replace(self.destination, self.target)
        fsync_directory(self.target.parent)
        if self.destination.parent != self.target.parent:
            fsync_directory(self.destination.parent)


@dataclass
class RemovePathOperation(TransactionOperation):
    backup_path: Path = Path(".")

    @property
    def kind(self) -> OperationKind:
        return OperationKind.REMOVE_PATH

    def prepare(self) -> None:
        super().prepare()
        ensure_within_root(self.backup_path, self.allowed_root)
        if not (self.target.exists() or self.target.is_symlink()):
            raise OperationExecutionError(self.operation_id, "remove target does not exist")
        if self.backup_path.exists() or self.backup_path.is_symlink():
            raise OperationExecutionError(self.operation_id, "remove backup path already exists")
        if not self.backup_path.parent.is_dir():
            raise OperationExecutionError(self.operation_id, "remove backup parent does not exist")
        if self.target.parent.stat().st_dev != self.backup_path.parent.stat().st_dev:
            raise OperationExecutionError(self.operation_id, "EXACT remove requires same-filesystem backup path")

    def journal_data(self) -> dict[str, Any]:
        data = super().journal_data()
        data["backup_path"] = str(self.backup_path)
        data["before_fingerprint"] = fingerprint_path(self.target)
        return data

    def apply(self) -> None:
        os.replace(self.target, self.backup_path)
        fsync_directory(self.target.parent)
        if self.backup_path.parent != self.target.parent:
            fsync_directory(self.backup_path.parent)

    def rollback(self) -> None:
        if self.target.exists() or self.target.is_symlink():
            raise OperationExecutionError(self.operation_id, "remove rollback target is occupied")
        if not (self.backup_path.exists() or self.backup_path.is_symlink()):
            raise OperationExecutionError(self.operation_id, "remove rollback backup is missing")
        os.replace(self.backup_path, self.target)
        fsync_directory(self.target.parent)
        if self.backup_path.parent != self.target.parent:
            fsync_directory(self.backup_path.parent)


class TransactionExecutor:
    def __init__(self, journal: WriteAheadJournal) -> None:
        self.journal = journal

    def execute(self, operation: TransactionOperation) -> None:
        if operation.safety.reversibility == Reversibility.NONE:
            raise OperationExecutionError(
                operation.operation_id,
                "mutation primitive refuses Reversibility.NONE at the safety-kernel stage",
            )

        operation.prepare()
        operation.verify_before_mutation()
        self.journal.append(
            operation_id=operation.operation_id,
            state=OperationState.INTENT,
            kind=operation.kind.value,
            target=str(operation.target),
            data=operation.journal_data(),
        )
        self.journal.append(
            operation_id=operation.operation_id,
            state=OperationState.STARTED,
            kind=operation.kind.value,
            target=str(operation.target),
        )

        try:
            # Recheck after durable STARTED journal entry so changes that occur
            # during prepare/journaling still cannot be silently clobbered.
            operation.verify_before_mutation()
            operation.apply()
        except Exception as exc:
            self.journal.append(
                operation_id=operation.operation_id,
                state=OperationState.FAILED,
                kind=operation.kind.value,
                target=str(operation.target),
                data={"error_type": type(exc).__name__, "error": str(exc)},
            )
            raise

        self.journal.append(
            operation_id=operation.operation_id,
            state=OperationState.COMPLETED,
            kind=operation.kind.value,
            target=str(operation.target),
            data={"after_fingerprint": fingerprint_path(operation.target)},
        )

    def rollback(self, operation: TransactionOperation) -> None:
        self.journal.append(
            operation_id=operation.operation_id,
            state=OperationState.ROLLBACK_STARTED,
            kind=operation.kind.value,
            target=str(operation.target),
        )
        try:
            operation.rollback()
        except Exception as exc:
            self.journal.append(
                operation_id=operation.operation_id,
                state=OperationState.ROLLBACK_FAILED,
                kind=operation.kind.value,
                target=str(operation.target),
                data={"error_type": type(exc).__name__, "error": str(exc)},
            )
            raise
        self.journal.append(
            operation_id=operation.operation_id,
            state=OperationState.ROLLED_BACK,
            kind=operation.kind.value,
            target=str(operation.target),
        )


def _durable_write_bytes(path: Path, content: bytes, *, mode: int = 0o600) -> None:
    temporary = path.with_name(f".{path.name}.tmp-{uuid.uuid4().hex[:8]}")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        fsync_directory(path.parent)
    finally:
        if temporary.exists() or temporary.is_symlink():
            temporary.unlink(missing_ok=True)


def _durable_copy_file(source: Path, destination: Path) -> None:
    temporary = destination.with_name(f".{destination.name}.tmp-{uuid.uuid4().hex[:8]}")
    try:
        shutil.copy2(source, temporary, follow_symlinks=False)
        with temporary.open("rb") as handle:
            os.fsync(handle.fileno())
        os.replace(temporary, destination)
        fsync_directory(destination.parent)
    finally:
        if temporary.exists() or temporary.is_symlink():
            temporary.unlink(missing_ok=True)
