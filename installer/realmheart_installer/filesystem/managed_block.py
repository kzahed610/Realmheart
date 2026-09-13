"""Transaction-aware managed blocks for shared user configuration files."""

from __future__ import annotations

import stat
from dataclasses import dataclass
from pathlib import Path

from ..errors import ManagedBlockConflictError
from ..models import OperationSafety, Reversibility
from ..transaction.operations import WriteFileOperation, new_operation_id
from ..transaction.preconditions import capture_path_precondition


@dataclass(frozen=True)
class ManagedBlock:
    begin_marker: str
    end_marker: str
    body: str

    def encoded(self) -> bytes:
        body = self.body.rstrip("\r\n")
        return (
            f"{self.begin_marker}\n"
            f"{body}\n"
            f"{self.end_marker}\n"
        ).encode("utf-8")


def plan_ensure_managed_block(
    *,
    target: Path,
    allowed_root: Path,
    preimage_dir: Path,
    block: ManagedBlock,
    default_mode: int = 0o600,
) -> WriteFileOperation | None:
    """Plan an idempotent whole-file atomic replace for one managed block.

    Non-managed bytes are preserved exactly when replacing an existing valid
    block. Missing/duplicate/partial markers are never normalized by guessing.
    """

    target = Path(target)
    current = target.read_bytes() if target.exists() and not target.is_symlink() else b""
    if target.is_symlink():
        # Shared-config symlink semantics need a deliberate product decision;
        # do not silently follow a link into another config tree at this layer.
        raise ManagedBlockConflictError(str(target), "shared config target is a symlink")

    desired, changed = ensure_managed_block_bytes(current, block, target=str(target))
    if not changed:
        return None

    if target.exists():
        mode = stat.S_IMODE(target.stat(follow_symlinks=False).st_mode)
    else:
        mode = default_mode

    operation_id = new_operation_id()
    preimage = Path(preimage_dir) / f"{operation_id}.preimage"
    return WriteFileOperation(
        target=target,
        allowed_root=Path(allowed_root),
        safety=OperationSafety(
            Reversibility.EXACT,
            capture_path_precondition(target),
            str(preimage),
        ),
        operation_id=operation_id,
        content=desired,
        mode=mode,
        preimage_path=preimage,
    )



def plan_remove_managed_block(
    *,
    target: Path,
    allowed_root: Path,
    preimage_dir: Path,
    block: ManagedBlock,
) -> WriteFileOperation | None:
    """Plan surgical removal of exactly one managed block.

    Only bytes between the canonical begin/end markers are removed.  All
    unrelated current user bytes are preserved verbatim, and malformed/partial
    marker state fails closed instead of guessing.
    """

    target = Path(target)
    if target.is_symlink():
        raise ManagedBlockConflictError(str(target), "shared config target is a symlink")
    if not target.exists():
        return None

    current = target.read_bytes()
    desired, changed = remove_managed_block_bytes(current, block, target=str(target))
    if not changed:
        return None

    mode = stat.S_IMODE(target.stat(follow_symlinks=False).st_mode)
    operation_id = new_operation_id()
    preimage = Path(preimage_dir) / f"{operation_id}.preimage"
    return WriteFileOperation(
        target=target,
        allowed_root=Path(allowed_root),
        safety=OperationSafety(
            Reversibility.EXACT,
            capture_path_precondition(target),
            str(preimage),
        ),
        operation_id=operation_id,
        content=desired,
        mode=mode,
        preimage_path=preimage,
    )


def remove_managed_block_bytes(current: bytes, block: ManagedBlock, *, target: str = "<memory>") -> tuple[bytes, bool]:
    """Remove one canonical managed block without normalizing neighbor bytes."""

    begin = block.begin_marker.encode("utf-8")
    end = block.end_marker.encode("utf-8")
    lines = current.splitlines(keepends=True)
    begin_indices = [index for index, line in enumerate(lines) if line.rstrip(b"\r\n") == begin]
    end_indices = [index for index, line in enumerate(lines) if line.rstrip(b"\r\n") == end]

    if not begin_indices and not end_indices:
        return current, False
    if len(begin_indices) != 1 or len(end_indices) != 1:
        raise ManagedBlockConflictError(
            target,
            f"expected exactly one begin/end marker, found begin={len(begin_indices)} end={len(end_indices)}",
        )

    begin_index = begin_indices[0]
    end_index = end_indices[0]
    if end_index < begin_index:
        raise ManagedBlockConflictError(target, "end marker appears before begin marker")

    desired = b"".join(lines[:begin_index]) + b"".join(lines[end_index + 1 :])
    return desired, desired != current

def ensure_managed_block_bytes(current: bytes, block: ManagedBlock, *, target: str = "<memory>") -> tuple[bytes, bool]:
    begin = block.begin_marker.encode("utf-8")
    end = block.end_marker.encode("utf-8")
    desired_block = block.encoded()

    lines = current.splitlines(keepends=True)
    begin_indices = [index for index, line in enumerate(lines) if line.rstrip(b"\r\n") == begin]
    end_indices = [index for index, line in enumerate(lines) if line.rstrip(b"\r\n") == end]

    if not begin_indices and not end_indices:
        if not current:
            desired = desired_block
        else:
            separator = b"\n" if current.endswith((b"\n", b"\r")) else b"\n\n"
            desired = current + separator + desired_block
        return desired, desired != current

    if len(begin_indices) != 1 or len(end_indices) != 1:
        raise ManagedBlockConflictError(
            target,
            f"expected exactly one begin/end marker, found begin={len(begin_indices)} end={len(end_indices)}",
        )

    begin_index = begin_indices[0]
    end_index = end_indices[0]
    if end_index < begin_index:
        raise ManagedBlockConflictError(target, "end marker appears before begin marker")

    prefix = b"".join(lines[:begin_index])
    suffix = b"".join(lines[end_index + 1 :])
    desired = prefix + desired_block + suffix
    return desired, desired != current
