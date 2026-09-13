"""Same-filesystem full-tree staging/swap for Realmheart-owned config trees."""

from __future__ import annotations

import os
import shutil
import stat
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from ..errors import StagingError
from ..models import OperationSafety, Reversibility
from ..transaction.journal import WriteAheadJournal
from ..transaction.operations import MovePathOperation, TransactionExecutor
from ..transaction.preconditions import capture_path_precondition
from .backup import _copy_path_symlink_safe
from .compare import fingerprint_path


@dataclass(frozen=True)
class FullTreeStage:
    target: Path
    staging: Path
    old: Path
    active_precondition_fingerprint: str
    staging_fingerprint: str
    preserved_relative_paths: tuple[str, ...]


def prepare_full_tree_stage(
    *,
    release_tree: Path,
    target: Path,
    transaction_id: str,
    preserve_relative_paths: Iterable[str] = ("custom",),
) -> FullTreeStage:
    """Build a validated sibling staging tree without touching the active target."""

    release_tree = Path(release_tree)
    target = Path(target)
    if not release_tree.is_dir() or release_tree.is_symlink():
        raise StagingError(f"Release tree is not a normal directory: {release_tree}")
    if not target.parent.is_dir():
        raise StagingError(f"Target parent does not exist: {target.parent}")

    staging = target.parent / f".realmheart-{target.name}-staging-{transaction_id}"
    old = target.parent / f".realmheart-{target.name}-old-{transaction_id}"
    for path in (staging, old):
        if path.exists() or path.is_symlink():
            raise StagingError(f"Refusing staging collision at {path}", code="RH_STAGING_COLLISION")

    preserved = tuple(preserve_relative_paths)
    for relative in preserved:
        _validate_relative_preservation_path(relative)

    try:
        _copy_path_symlink_safe(release_tree, staging)
        for relative in preserved:
            active_user_path = target / relative
            staged_default_path = staging / relative
            if active_user_path.exists() or active_user_path.is_symlink():
                _overlay_user_tree(active_user_path, staged_default_path)

        if not any(staging.iterdir()):
            raise StagingError("Staged tree is unexpectedly empty")

        active_before = fingerprint_path(target)
        staged_fingerprint = fingerprint_path(staging)
        return FullTreeStage(
            target=target,
            staging=staging,
            old=old,
            active_precondition_fingerprint=active_before,
            staging_fingerprint=staged_fingerprint,
            preserved_relative_paths=preserved,
        )
    except Exception:
        if staging.exists() and staging.is_dir() and not staging.is_symlink():
            shutil.rmtree(staging)
        elif staging.exists() or staging.is_symlink():
            staging.unlink()
        raise


class FullTreeSwap:
    """Journaled active→old, staging→active swap.

    The old tree is deliberately retained after success. Transaction finalization
    decides when it is safe to remove it after persistent backup requirements are
    satisfied.
    """

    def __init__(self, stage: FullTreeStage, journal: WriteAheadJournal) -> None:
        self.stage = stage
        self.journal = journal
        self.executor = TransactionExecutor(journal)
        self.operations: list[MovePathOperation] = []

    def execute(self) -> None:
        target = self.stage.target
        root = target.parent
        self.operations = []

        if target.exists() or target.is_symlink():
            active_move = MovePathOperation(
                target=target,
                destination=self.stage.old,
                allowed_root=root,
                safety=OperationSafety(
                    Reversibility.EXACT,
                    capture_path_precondition(target),
                    str(self.stage.old),
                ),
            )
            if active_move.safety.precondition.expected_fingerprint != self.stage.active_precondition_fingerprint:
                raise StagingError(
                    "Active tree changed after staging was prepared; refusing takeover",
                    code="RH_STAGING_ACTIVE_DRIFT",
                )
            self.executor.execute(active_move)
            self.operations.append(active_move)

        staged_move = MovePathOperation(
            target=self.stage.staging,
            destination=target,
            allowed_root=root,
            safety=OperationSafety(
                Reversibility.EXACT,
                capture_path_precondition(self.stage.staging),
                str(self.stage.staging),
            ),
        )
        if staged_move.safety.precondition.expected_fingerprint != self.stage.staging_fingerprint:
            raise StagingError(
                "Staging tree changed after validation; refusing activation",
                code="RH_STAGING_PAYLOAD_DRIFT",
            )
        self.executor.execute(staged_move)
        self.operations.append(staged_move)

    def rollback(self) -> None:
        for operation in reversed(self.operations):
            self.executor.rollback(operation)


def _overlay_user_tree(user_path: Path, staged_path: Path) -> None:
    """Overlay the user's preservation island over new release defaults.

    User-owned same-path entries win. Release-only entries remain, which seeds
    newly introduced defaults without overwriting existing user content.
    """

    user_st = user_path.lstat()
    if not stat.S_ISDIR(user_st.st_mode) or stat.S_ISLNK(user_st.st_mode):
        _replace_with_copy(user_path, staged_path)
        return

    if staged_path.exists() or staged_path.is_symlink():
        if staged_path.is_symlink() or not staged_path.is_dir():
            _remove_path(staged_path)
            staged_path.mkdir(mode=stat.S_IMODE(user_st.st_mode))
    else:
        staged_path.mkdir(mode=stat.S_IMODE(user_st.st_mode))

    with os.scandir(user_path) as iterator:
        entries = sorted(iterator, key=lambda item: os.fsencode(item.name))
    for entry in entries:
        source_child = Path(entry.path)
        destination_child = staged_path / entry.name
        child_st = entry.stat(follow_symlinks=False)
        if stat.S_ISDIR(child_st.st_mode) and not stat.S_ISLNK(child_st.st_mode):
            _overlay_user_tree(source_child, destination_child)
        else:
            _replace_with_copy(source_child, destination_child)


def _replace_with_copy(source: Path, destination: Path) -> None:
    if destination.exists() or destination.is_symlink():
        _remove_path(destination)
    _copy_path_symlink_safe(source, destination)


def _remove_path(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)
    else:
        path.unlink(missing_ok=True)


def _validate_relative_preservation_path(value: str) -> None:
    path = Path(value)
    if not value or path.is_absolute() or ".." in path.parts or path == Path("."):
        raise StagingError(f"Unsafe preservation path: {value!r}", code="RH_STAGING_INVALID_PRESERVE_PATH")
